import json
import os
import struct
from datetime import datetime, UTC
import time
import paho.mqtt.client as mqtt
import queue
import threading

import gzip
import math

from fault_logger import log_fault_events

## Addition for ACK
from node_registry import register_serial, serial_from_topic, get_node_by_serial
from settings_store import (
    apply_accelerometer_config_ack,
    update_accelerometer_runtime_state,
)

from raw_backup import write_raw, close_all as raw_backup_close_all

BROKER_IP = "localhost"
PORT = 1883
TOPIC = "wind_turbine/+/data"
STATUS_TOPIC = "wind_turbine/+/status"  # MQTT status topic for ACK/state updates
FAULT_TOPIC = "wind_turbine/+/faults"

DATA_DIR  = "/mnt/ssd/data"
SSD_MOUNT = "/mnt/ssd"

# SSD state tracking — re-evaluated on every write so mid-run unmounts
# and re-mounts are detected automatically.
_ssd_ok        = False   # True when SSD is mounted and writable
_ssd_last_warn = 0.0     # monotonic time of last SSD-unavailable warning
_SSD_WARN_INTERVAL = 30.0  # seconds between repeated SSD warnings

DELTA_NULL_THRESHOLD = 1
INT32_NAN_SENTINEL = -2147483648

CHANGED_TS = 0x01
CHANGED_X = 0x02
CHANGED_Y = 0x04
CHANGED_Z = 0x08
CHANGED_NAN_X = 0x10
CHANGED_NAN_Y = 0x20
CHANGED_NAN_Z = 0x40
CHANGED_NAN_TEMP = CHANGED_NAN_X

def _check_ssd() -> bool:
    """Return True if the SSD is mounted and the data directory is writable.
    Updates the module-level _ssd_ok flag and emits rate-limited warnings.
    """
    global _ssd_ok, _ssd_last_warn
    mounted   = os.path.isdir(SSD_MOUNT) and os.path.ismount(SSD_MOUNT)
    if mounted:
        try:
            os.makedirs(DATA_DIR, exist_ok=True)
            # Quick write-test: check free space >= 10 MB
            stat = os.statvfs(SSD_MOUNT)
            free_mb = stat.f_bavail * stat.f_frsize / (1024 * 1024)
            if free_mb < 10:
                _warn_ssd(f"SSD critically low on space: {free_mb:.1f} MB free")
                _ssd_ok = False
                return False
            if not _ssd_ok:
                print("[SSD] Storage available — resuming writes.")
            _ssd_ok = True
            return True
        except OSError as e:
            _warn_ssd(f"SSD mounted but not writable: {e}")
            _ssd_ok = False
            return False
    else:
        _warn_ssd("SSD not mounted at /mnt/ssd — sensor data will not be saved.")
        _ssd_ok = False
        return False


def _ssd_ok_reset():
    """Force the next _check_ssd() call to re-evaluate (called after write error)."""
    global _ssd_ok
    _ssd_ok = False


def _warn_ssd(msg: str):
    """Emit an SSD warning at most once per _SSD_WARN_INTERVAL seconds."""
    global _ssd_last_warn
    now = time.monotonic()
    if now - _ssd_last_warn >= _SSD_WARN_INTERVAL:
        _ssd_last_warn = now
        print(f"[SSD] WARNING: {msg}")

# Queue/backoff settings used by the MQTT consumer path.
QUEUE_MAX_SIZE = 1000
KEEPALIVE_S = 60
MAX_RECONNECT_DELAY_S = 30

# Perform initial SSD check at startup.
_check_ssd()

# -------------------------------------------------------------------
# Gzip compression settings
#
# Each hourly .bin file is written as plain binary while the hour is
# active. When the hour rolls over the file is compressed to .bin.gz
# (gzip level 4) and the original .bin is deleted.
#
# GZIP_LEVEL : 1 (fastest) … 9 (smallest). Level 4 is a good balance
#             between speed and size for binary sensor data.
# -------------------------------------------------------------------
GZIP_LEVEL = 4

# -------------------------------------------------------------------
# Packet format (JSON input):
#   {"a": [[t,x,y,z], ...], "i": [t,x,y,z], "T": [t,val]}
#   Each sensor carries its own ISO 8601 UTC timestamp string.
#
# Binary format — each sensor tracks its own timestamps independently.
# -------------------------------------------------------------------

TEMP_SCALE = 100        # 0.01 °C  -> int
ACCEL_SCALE = 10000     # 0.0001 g -> int
INCLIN_SCALE = 10000    # 0.0001 ° -> int
TS_SCALE = 1_000_000    # seconds  -> µs

FILE_FORMAT_VERSION = 3

MAX_DELTA_S = 60.0
INT16_MAX = 32767

TS_MIN = 1_577_836_800.0   # 2020-01-01
TS_MAX = 4_102_444_800.0   # 2100-01-01
_clock_warn_interval = 10.0
_last_clock_warn: dict[str, float] = {}

# -------------------------------------------------------------------
# Timestamp helpers
# -------------------------------------------------------------------

def parse_iso_timestamp(raw_t: str, node_id: str) -> float | None:
    """
    Parse an ISO 8601 UTC timestamp string to Unix seconds (float).
    Returns None if parsing fails or the timestamp is implausible.
    """
    try:
        ts = datetime.strptime(
            raw_t.rstrip("Z"), "%Y-%m-%dT%H:%M:%S.%f"
        ).replace(tzinfo=__import__("datetime").timezone.utc).timestamp()
    except (ValueError, AttributeError):
        now = time.monotonic()
        last = _last_clock_warn.get(node_id, 0.0)
        if now - last >= _clock_warn_interval:
            _last_clock_warn[node_id] = now
            print(f"[{node_id}] WARNING: cannot parse timestamp {raw_t!r} — packet dropped.")
        return None

    if not (TS_MIN <= ts <= TS_MAX):
        now = time.monotonic()
        last = _last_clock_warn.get(node_id, 0.0)
        if now - last >= _clock_warn_interval:
            _last_clock_warn[node_id] = now
            try:
                date_str = datetime.utcfromtimestamp(int(ts)).strftime("%Y-%m-%d")
            except Exception:
                date_str = "?"
            print(
                f"[{node_id}] WARNING: implausible timestamp {raw_t!r} "
                f"({date_str}) — clock not synced, packet dropped."
            )
        return None

    return ts


def normalise_sensor_timestamps(data: dict, node_id: str) -> bool:
    """
    Parse ISO 8601 timestamps in-place to Unix seconds (float).
    Returns False if any sensor timestamp is invalid.
    """
    if "a" in data:
        for sample in data["a"]:
            ts = parse_iso_timestamp(str(sample[0]), node_id)
            if ts is None:
                return False
            sample[0] = ts

    if "i" in data:
        for sample in data["i"]:
            ts = parse_iso_timestamp(str(sample[0]), node_id)
            if ts is None:
                return False
            sample[0] = ts

    if "T" in data:
        ts = parse_iso_timestamp(str(data["T"][0]), node_id)
        if ts is None:
            return False
        data["T"][0] = ts

    return True


def now_iso() -> str:
    return datetime.now(UTC).isoformat().replace("+00:00", "Z")


# -------------------------------------------------------------------
# Per-node encoding state
# -------------------------------------------------------------------

node_state: dict = {}
data_buffer: queue.Queue = queue.Queue(maxsize=QUEUE_MAX_SIZE)


def _fresh_state() -> dict:
    """Create fresh per-node encoder state."""
    return {
        "accel": {"ts_us": 0, "xyz_prev": [0, 0, 0]},
        "inclin": {"ts_us": 0, "xyz_prev": [0, 0, 0]},
        "temp": {"ts_us": 0, "val_prev": 0},
        "file_hour": None,
        "is_first": True,
        "header_written": False,
    }


def get_hourly_filepath(node_id: str) -> tuple[str, str]:
    """Build the active hourly .bin path for one node."""
    hour_str = datetime.now().strftime("%Y%m%d_%H")
    return hour_str, os.path.join(DATA_DIR, f"data_{node_id}_{hour_str}.bin")


# -------------------------------------------------------------------
# Overflow / clip guards
# -------------------------------------------------------------------

def needs_absolute_record(data: dict, state: dict) -> tuple[bool, str]:
    """Return (True, reason) if any sensor would overflow delta_ts or clip int16."""
    if "a" in data and len(data["a"]) > 0:
        ts_s = float(data["a"][0][0])
        gap = ts_s - state["accel"]["ts_us"] / TS_SCALE
        if abs(gap) > MAX_DELTA_S:
            return True, f"accel timestamp gap {gap:.1f} s"

        prev = list(state["accel"]["xyz_prev"])
        for s in data["a"]:
            for idx, (raw, scale, name) in enumerate(((s[1], ACCEL_SCALE, "x"), (s[2], ACCEL_SCALE, "y"), (s[3], ACCEL_SCALE, "z"))):
                if _is_nan_value(raw):
                    continue
                cur = int(float(raw) * scale)
                d = cur - prev[idx]
                if abs(d) >= INT16_MAX:
                    return True, f"accel {name} delta {d} would clip int16"
                prev[idx] = cur

    if "i" in data and len(data["i"]) > 0:
        ts_s = float(data["i"][0][0])
        gap = ts_s - state["inclin"]["ts_us"] / TS_SCALE
        if abs(gap) > MAX_DELTA_S:
            return True, f"inclin timestamp gap {gap:.1f} s"
        prev = list(state["inclin"]["xyz_prev"])
        for s in data["i"]:
            for idx, (raw, scale, name) in enumerate(((s[1], INCLIN_SCALE, "roll"), (s[2], INCLIN_SCALE, "pitch"), (s[3], INCLIN_SCALE, "yaw"))):
                if _is_nan_value(raw):
                    continue
                cur = int(float(raw) * scale)
                d = cur - prev[idx]
                if abs(d) >= INT16_MAX:
                    return True, f"inclin {name} delta {d} would clip int16"
                prev[idx] = cur

    if "T" in data:
        ts_s = float(data["T"][0])
        ts_us_new = int(ts_s * TS_SCALE)
        if ts_us_new != state["temp"]["ts_us"]:
            gap = ts_s - state["temp"]["ts_us"] / TS_SCALE
            if abs(gap) > MAX_DELTA_S:
                return True, f"temp timestamp gap {gap:.1f} s"
        if len(data["T"]) > 1 and not _is_nan_value(data["T"][1]):
            d = int(float(data["T"][1]) * TEMP_SCALE) - state["temp"]["val_prev"]
            if abs(d) >= INT16_MAX:
                return True, f"temp delta {d} would clip int16"

    return False, ""


# -------------------------------------------------------------------
# Timestamp encoding helpers
# -------------------------------------------------------------------

def _abs_ts_bytes(ts_s: float, ss: dict) -> bytes:
    """Encode absolute timestamp and update sensor state."""
    ts_us = int(ts_s * TS_SCALE)
    ss["ts_us"] = ts_us
    return struct.pack("<q", ts_us)


def _pack_delta(value: int) -> bytes:
    """Clamp and pack a signed delta as int16."""
    return struct.pack("<h", max(-32768, min(32767, value)))

def _is_nan_value(value) -> bool:
    try:
        return math.isnan(float(value))
    except (TypeError, ValueError):
        return False

def _apply_null_threshold(delta: int, threshold: int = DELTA_NULL_THRESHOLD) -> int:
    return 0 if abs(delta) <= threshold else delta

def _encode_abs_component(value, scale: int):
    if _is_nan_value(value):
        return INT32_NAN_SENTINEL, False
    return int(float(value) * scale), True


# -------------------------------------------------------------------
# ABSOLUTE record encoder
# -------------------------------------------------------------------

def encode_first_record(data: dict, state: dict) -> bytes:
    """Encode an ABSOLUTE record."""
    header = 0
    body = bytearray()

    if "a" in data and len(data["a"]) > 0:
        header |= 0x01
        samples = data["a"]
        body += struct.pack("<B", len(samples))
        prev = list(state["accel"]["xyz_prev"])
        for s in samples:
            xi, hx = _encode_abs_component(s[1], ACCEL_SCALE)
            yi, hy = _encode_abs_component(s[2], ACCEL_SCALE)
            zi, hz = _encode_abs_component(s[3], ACCEL_SCALE)
            body += _abs_ts_bytes(float(s[0]), state["accel"])
            body += struct.pack("<iii", xi, yi, zi)
            if hx: prev[0] = xi
            if hy: prev[1] = yi
            if hz: prev[2] = zi
        state["accel"]["xyz_prev"] = prev

    if "i" in data and len(data["i"]) > 0:
        header |= 0x02
        samples = data["i"]
        body += struct.pack("<B", len(samples))
        prev = list(state["inclin"]["xyz_prev"])
        for s in samples:
            ri, hr = _encode_abs_component(s[1], INCLIN_SCALE)
            pi, hp = _encode_abs_component(s[2], INCLIN_SCALE)
            yi, hy = _encode_abs_component(s[3], INCLIN_SCALE)
            body += _abs_ts_bytes(float(s[0]), state["inclin"])
            body += struct.pack("<iii", ri, pi, yi)
            if hr: prev[0] = ri
            if hp: prev[1] = pi
            if hy: prev[2] = yi
        state["inclin"]["xyz_prev"] = prev

    if "T" in data:
        header |= 0x04
        tv = data["T"]
        vi, have_val = _encode_abs_component(tv[1], TEMP_SCALE)
        body += _abs_ts_bytes(float(tv[0]), state["temp"])
        body += struct.pack("<i", vi)
        if have_val:
            state["temp"]["val_prev"] = vi

    return struct.pack("<BB", 0xFF, header) + bytes(body)


# -------------------------------------------------------------------
# DELTA record encoder
# -------------------------------------------------------------------

def encode_delta_record(data: dict, state: dict) -> bytes:
    """Encode a DELTA record using changed-bit masks plus explicit NaN flags."""
    header = 0
    body = bytearray()

    if "a" in data and len(data["a"]) > 0:
        header |= 0x01
        samples = data["a"]
        body += struct.pack("<B", len(samples))
        prev = list(state["accel"]["xyz_prev"])
        ss = state["accel"]

        for s in samples:
            nan_x = _is_nan_value(s[1])
            nan_y = _is_nan_value(s[2])
            nan_z = _is_nan_value(s[3])

            ts_us_new = int(float(s[0]) * TS_SCALE)
            delta_us = ts_us_new - ss["ts_us"]

            changed = 0
            if delta_us != 0:
                changed |= 0x01

            if nan_x:
                dx = 0
                changed |= CHANGED_NAN_X
            else:
                xi = int(float(s[1]) * ACCEL_SCALE)
                dx = _apply_null_threshold(xi - prev[0])
                if dx != 0:
                    changed |= 0x02

            if nan_y:
                dy = 0
                changed |= CHANGED_NAN_Y
            else:
                yi = int(float(s[2]) * ACCEL_SCALE)
                dy = _apply_null_threshold(yi - prev[1])
                if dy != 0:
                    changed |= 0x04

            if nan_z:
                dz = 0
                changed |= CHANGED_NAN_Z
            else:
                zi = int(float(s[3]) * ACCEL_SCALE)
                dz = _apply_null_threshold(zi - prev[2])
                if dz != 0:
                    changed |= 0x08

            body += struct.pack("<B", changed)
            if changed & 0x01:
                body += struct.pack("<i", delta_us)
            if changed & 0x02:
                body += _pack_delta(dx)
            if changed & 0x04:
                body += _pack_delta(dy)
            if changed & 0x08:
                body += _pack_delta(dz)

            ss["ts_us"] = ts_us_new
            if not nan_x:
                prev[0] += dx
            if not nan_y:
                prev[1] += dy
            if not nan_z:
                prev[2] += dz

        state["accel"]["xyz_prev"] = prev

    if "i" in data and len(data["i"]) > 0:
        header |= 0x02
        samples = data["i"]
        body += struct.pack("<B", len(samples))
        prev = list(state["inclin"]["xyz_prev"])
        ss = state["inclin"]

        for s in samples:
            nan_r = _is_nan_value(s[1])
            nan_p = _is_nan_value(s[2])
            nan_y = _is_nan_value(s[3])

            ts_us_new = int(float(s[0]) * TS_SCALE)
            delta_us = ts_us_new - ss["ts_us"]

            changed = 0
            if delta_us != 0:
                changed |= 0x01

            if nan_r:
                dr = 0
                changed |= CHANGED_NAN_X
            else:
                ri = int(float(s[1]) * INCLIN_SCALE)
                dr = _apply_null_threshold(ri - prev[0])
                if dr != 0:
                    changed |= 0x02

            if nan_p:
                dp = 0
                changed |= CHANGED_NAN_Y
            else:
                pi = int(float(s[2]) * INCLIN_SCALE)
                dp = _apply_null_threshold(pi - prev[1])
                if dp != 0:
                    changed |= 0x04

            if nan_y:
                dy = 0
                changed |= CHANGED_NAN_Z
            else:
                yi = int(float(s[3]) * INCLIN_SCALE)
                dy = _apply_null_threshold(yi - prev[2])
                if dy != 0:
                    changed |= 0x08

            body += struct.pack("<B", changed)
            if changed & 0x01:
                body += struct.pack("<i", delta_us)
            if changed & 0x02:
                body += _pack_delta(dr)
            if changed & 0x04:
                body += _pack_delta(dp)
            if changed & 0x08:
                body += _pack_delta(dy)

            ss["ts_us"] = ts_us_new
            if not nan_r:
                prev[0] += dr
            if not nan_p:
                prev[1] += dp
            if not nan_y:
                prev[2] += dy

        state["inclin"]["xyz_prev"] = prev

    if "T" in data:
        tv = data["T"]
        ss = state["temp"]
        ts_us_new = int(float(tv[0]) * TS_SCALE)
        delta_us = ts_us_new - ss["ts_us"]
        val_is_nan = _is_nan_value(tv[1])

        changed = 0
        if delta_us != 0:
            changed |= 0x01
        if val_is_nan:
            changed |= CHANGED_NAN_TEMP
            dt = 0
        else:
            vi = int(float(tv[1]) * TEMP_SCALE)
            dt = _apply_null_threshold(vi - ss["val_prev"])
            if dt != 0:
                changed |= 0x02

        is_frozen = (changed == 0)
        ss["ts_us"] = ts_us_new

        if not is_frozen:
            header |= 0x04
            body += struct.pack("<B", changed)
            if changed & 0x01:
                body += struct.pack("<i", delta_us)
            if changed & 0x02:
                body += _pack_delta(dt)
            if not val_is_nan:
                ss["val_prev"] += dt

    return struct.pack("<B", header) + bytes(body)


# -------------------------------------------------------------------
# Write record
# -------------------------------------------------------------------

def compress_and_replace(node_id: str, bin_path: str):
    """Compress a closed .bin file to .bin.gz (gzip level 4) and delete
    the original.  Skips silently if the SSD is unavailable.
    """
    if not _check_ssd():
        print(f"[{node_id}] Skipping compression of {os.path.basename(bin_path)} — SSD unavailable")
        return
    gz_path = bin_path + ".gz"   # data_N01_20260322_14.bin → data_N01_20260322_14.bin.gz
    try:
        original_size = os.path.getsize(bin_path)
        with open(bin_path, "rb") as f_in, \
             gzip.open(gz_path, "wb", compresslevel=GZIP_LEVEL) as f_out:
            while True:
                chunk = f_in.read(65536)
                if not chunk:
                    break
                f_out.write(chunk)

        compressed_size = os.path.getsize(gz_path)
        ratio = compressed_size / original_size * 100 if original_size else 0
        os.remove(bin_path)

        print(
            f"[{node_id}] Compressed {os.path.basename(bin_path)} → "
            f"{os.path.basename(gz_path)} "
            f"({original_size:,} → {compressed_size:,} bytes, {ratio:.1f}%)"
        )
    except Exception as e:
        print(f"[{node_id}] WARNING: compression failed for {bin_path}: {e}")


def write_record(node_id: str, data: dict):
    """Write one normalized packet into the active hourly binary file.
    Skips the write silently (with rate-limited warning) if the SSD is
    unavailable. Re-checks SSD availability on every call so recovery
    after a remount is automatic.
    """
    if not _check_ssd():
        return   # SSD unavailable — packet dropped, warning already emitted

    hour_str, filepath = get_hourly_filepath(node_id)

    if node_id not in node_state:
        node_state[node_id] = _fresh_state()
    state = node_state[node_id]

    if state["file_hour"] != hour_str:
        if state["file_hour"] is not None:
            old_bin = os.path.join(DATA_DIR, f"data_{node_id}_{state['file_hour']}.bin")
            if os.path.exists(old_bin):
                compress_and_replace(node_id, old_bin)

        state["file_hour"] = hour_str
        state["is_first"] = True
        state["header_written"] = False
        print(f"[{node_id}] New hourly file: {filepath}")

    if not state["is_first"]:
        force_abs, reason = needs_absolute_record(data, state)
        if force_abs:
            print(f"[{node_id}] Forcing ABSOLUTE record: {reason}")
            state["is_first"] = True

    if state["is_first"]:
        record = encode_first_record(data, state)
        state["is_first"] = False
    else:
        record = encode_delta_record(data, state)

    try:
        with open(filepath, "ab") as f:
            if not state["header_written"]:
                f.write(struct.pack("<B", FILE_FORMAT_VERSION))
                state["header_written"] = True
            f.write(record)
    except OSError as e:
        # Disk full, I/O error, or SSD unmounted mid-write.
        # Mark the file as needing a fresh header on the next successful write
        # so the file is not left in a partially-written state.
        state["header_written"] = False
        state["is_first"]       = True   # force ABSOLUTE on recovery
        _ssd_ok_reset()
        _warn_ssd(f"write failed for {node_id}: {e}")


# -------------------------------------------------------------------
# Consumer thread
# -------------------------------------------------------------------

def consumer_worker():
    """Background worker that writes queued sensor packets to disk.
    Also runs a periodic SSD health check every 60 s so unmounts are
    detected even during quiet periods.
    """
    _SSD_CHECK_INTERVAL = 60.0
    last_ssd_check = time.monotonic()

    while True:
        try:
            node_id, data = data_buffer.get(timeout=_SSD_CHECK_INTERVAL)
        except queue.Empty:
            # No packets — just run the periodic health check and loop.
            _check_ssd()
            last_ssd_check = time.monotonic()
            continue

        try:
            write_record(node_id, data)
        except Exception as e:
            print(f"Error writing record for {node_id}: {e}")
        finally:
            data_buffer.task_done()

        # Periodic health check during active writes.
        now = time.monotonic()
        if now - last_ssd_check >= _SSD_CHECK_INTERVAL:
            _check_ssd()
            last_ssd_check = now


consumer_thread = threading.Thread(target=consumer_worker, daemon=True)
consumer_thread.start()


# -------------------------------------------------------------------
# Status/ACK handling
# -------------------------------------------------------------------

def handle_status_message(topic: str, payload_bytes: bytes) -> None:
    """Process node status messages and update backend config/runtime state."""
    try:
        serial = serial_from_topic(topic)

        node = get_node_by_serial(serial, timeout_seconds=60)
        if node is None:
            print(f"[status] Node not found for serial {serial}")
            return

        payload = json.loads(payload_bytes.decode())
        current_state = str(payload.get("state") or "unknown")
        acked_at = now_iso()

        seq_ack = payload.get("seq_ack")
        applied = bool(payload.get("applied"))

        has_full_config = all(
            key in payload for key in ("odr_index", "range", "hpf_corner")
        )

        if applied and seq_ack is not None and has_full_config:
            apply_accelerometer_config_ack(
                node_id=node["node_id"],
                odr_index=int(payload["odr_index"]),
                range_value=int(payload["range"]),
                hpf_corner=int(payload["hpf_corner"]),
                seq_ack=int(seq_ack),
                acked_at=acked_at,
                current_state=current_state,
            )
            print(f"[status] Applied config ACK for {serial} seq={seq_ack}")
            return

        update_accelerometer_runtime_state(
            node_id=node["node_id"],
            current_state=current_state,
            acked_at=acked_at,
        )
        print(f"[status] Updated runtime state for {serial}: {current_state}")

    except Exception as e:
        print(f"Failed to process status message on {topic}: {e}")


def handle_fault_message(topic: str, payload_bytes: bytes) -> None:
    """
    Process fault messages from wind_turbine/{serial}/faults.
    Expected payload:
    {"ts": "...", "f": 7} OR {"ts": "...", "f": [7,10]}
    """
    try:
        serial = serial_from_topic(topic)

        # keep node registry activity fresh for fault-only traffic too
        register_serial(serial)

        data = json.loads(payload_bytes.decode())

        ts = data.get("ts")
        faults = data.get("f")

        if ts is None or faults is None:
            print(f"[faults] Invalid payload from {serial}: {data}")
            return

        # normalize to list
        if isinstance(faults, int):
            faults = [faults]
        elif not isinstance(faults, list):
            print(f"[faults] Invalid fault format from {serial}: {faults}")
            return

        # build events [(code, ts), ...]
        fault_events = [(int(code), ts) for code in faults]

        # log directly
        log_fault_events(
            serial_number=serial,
            fault_events=fault_events,
        )

    except Exception as e:
        print(f"Error processing fault message on {topic}: {e}")


# -------------------------------------------------------------------
# MQTT callbacks
# -------------------------------------------------------------------

def on_connect(client, userdata, flags, reason_code, properties):
    """Subscribe to data, status, and fault topics on successful MQTT connect."""
    if reason_code == 0:
        print("[MQTT] Connected to broker")
    else:
        print(f"[MQTT] Connection failed: reason_code={reason_code} — will retry automatically")
        return

    client.subscribe(TOPIC)
    client.subscribe(STATUS_TOPIC)
    client.subscribe(FAULT_TOPIC)


def on_disconnect(client, userdata, flags, reason_code, properties):
    """Log disconnect events. Paho handles reconnect backoff."""
    if reason_code == 0:
        print("[MQTT] Disconnected cleanly")
    else:
        print(
            f"[MQTT] Unexpected disconnect (reason_code={reason_code}) — "
            f"paho will reconnect automatically with exponential back-off"
        )


def on_message(client, userdata, msg):
    """Handle incoming MQTT packets for status updates, fault events, and sensor data."""
    try:
        if msg.topic.endswith("/status"):
            handle_status_message(msg.topic, msg.payload)
            return

        if msg.topic.endswith("/faults"):
            handle_fault_message(msg.topic, msg.payload)
            return

        if not msg.topic.endswith("/data"):
            return

        topic_parts = msg.topic.split("/")
        node_id = topic_parts[1]

        # keep node registry activity fresh for data traffic
        register_serial(node_id)

        # Raw backup is written first so the original packet is preserved.
        write_raw(node_id, msg.payload)

        # Parse the JSON payload.
        data = json.loads(msg.payload.decode())

        # Normalize sensor timestamps only for binary storage.
        if not normalise_sensor_timestamps(data, node_id):
            return

        try:
            data_buffer.put_nowait((node_id, data))
        except queue.Full:
            print(
                f"[{node_id}] WARNING: queue full ({QUEUE_MAX_SIZE} items) — "
                f"packet dropped. Check if consumer is stalled (disk full/unmounted?)."
            )

    except Exception as e:
        print(f"Error processing MQTT message on {msg.topic}: {e}")


# -------------------------------------------------------------------
# Main
# -------------------------------------------------------------------

client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
client.on_connect = on_connect
client.on_disconnect = on_disconnect
client.on_message = on_message

# Enable automatic reconnect backoff.
client.reconnect_delay_set(min_delay=1, max_delay=MAX_RECONNECT_DELAY_S)

try:
    client.connect(BROKER_IP, PORT, keepalive=KEEPALIVE_S)
    client.loop_forever(retry_first_connection=True)
except Exception as e:
    print(f"[MQTT] Fatal connection error: {e}")
    raise
finally:
    raw_backup_close_all()