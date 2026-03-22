import json
import os
import struct
from datetime import datetime
import time
import paho.mqtt.client as mqtt
import queue
import threading

#from fault_logger import init_fault_db, log_fault_codes
import zstandard as zstd

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

DATA_DIR = "/mnt/ssd/data"
SSD_AVAILABLE = os.path.isdir("/mnt/ssd") and os.path.ismount("/mnt/ssd")

if SSD_AVAILABLE:
    os.makedirs(DATA_DIR, exist_ok=True)
else:
    print("[delta_encoder] SSD not mounted. Sensor data will not be saved.")

STATUS_TOPIC = "wind_turbine/+/status" ## ADDITION FOR ACK


# -------------------------------------------------------------------
# Zstandard compression settings
#
# Each hourly .bin file is written as plain binary while the hour is
# active (simple append, zero compression overhead on the hot path).
# When the hour rolls over the file is closed, compressed to .zst in
# one pass (best ratio — full file context), and the .bin is deleted.
#
# ZSTD_LEVEL : 1 (fastest) … 22 (smallest).
#              Level 3 is a good balance for sensor data.
# -------------------------------------------------------------------
ZSTD_LEVEL = 3

# -------------------------------------------------------------------
# Packet format (JSON input):
#   {"a": [[t,x,y,z], ...], "i": [t,x,y,z], "T": [t,val]}
#   Each sensor carries its own ISO 8601 UTC timestamp string,
#   e.g. "2026-03-21T21:26:57.519349Z".
#   parse_iso_timestamp() converts these to Unix seconds (float)
#   before any further processing.
#
# Binary format — each sensor tracks its OWN timestamps independently:
#
# ABSOLUTE record (first in file, or forced after gap/overflow/clip):
#   sentinel(0xFF) | header(B)
#   if accel  (bit0): n(B) + n × [ ts_abs(q)  x(i)   y(i)   z(i)  ]
#   if inclin (bit1):             [ ts_abs(q)  r(i)   p(i)   yaw(i)]
#   if temp   (bit2):             [ ts_abs(q)  val(i)              ]
#
# DELTA record:
#   header(B)
#   if accel  (bit0): n(B) + n × [ changed(B)  delta_ts?(i)  dx?(h)  dy?(h)  dz?(h)  ]
#   if inclin (bit1):             [ changed(B)  delta_ts?(i)  dr?(h)  dp?(h)  dyaw?(h) ]
#   if temp   (bit2):             [ changed(B)  delta_ts?(i)  dval?(h)                 ]
#
# changed byte bitmask (per sensor):
#   bit0 = timestamp changed  → delta_ts(i) present
#   bit1 = x/r/val changed    → dx/dr/dval(h) present
#   bit2 = y/p changed        → dy/dp(h) present   [accel/inclin]
#   bit3 = z/yaw changed      → dz/dyaw(h) present [accel/inclin]
# Fields with changed-bit=0 are omitted (null) — decoder keeps prev value.
# If changed==0x00 the sensor field is still present in header (for state
# tracking) but contributes 1 byte (the changed byte itself) to the record.
#
# ts_abs    : int64 µs  (absolute Unix µs)
# delta_ts  : int32 µs  (ts_current - ts_previous for that sensor)
# sensor values : int16, scaled (see ACCEL_SCALE / INCLIN_SCALE / TEMP_SCALE)
# -------------------------------------------------------------------

TEMP_SCALE    = 100        # 0.01 °C  → int16
ACCEL_SCALE   = 10000      # 0.0001 g → int16
INCLIN_SCALE  = 10000      # 0.0001 ° → int16
TS_SCALE      = 1_000_000  # seconds  → µs

# Binary file format version — first byte of every .bin file.
#   v1 = legacy (raw dod_ts, no changed byte)
#   v2 = current (changed byte prefix, simple delta_ts)
FILE_FORMAT_VERSION = 2

MAX_DELTA_S   = 60.0       # force ABSOLUTE if sensor timestamp gap > this
INT16_MAX     = 32767

TS_MIN          = 1_577_836_800.0   # 2020-01-01
TS_MAX          = 4_102_444_800.0   # 2100-01-01
_clock_warn_interval              = 10.0
_last_clock_warn: dict[str, float] = {}


# -------------------------------------------------------------------
# Timestamp helpers
# -------------------------------------------------------------------

def parse_iso_timestamp(raw_t: str, node_id: str) -> float | None:
    """
    Parse an ISO 8601 UTC timestamp string to Unix seconds (float).
    Accepts the format produced by the sensor firmware:
        "2026-03-21T21:26:57.519349Z"
    Returns None (and emits a rate-limited warning) if:
      - the string cannot be parsed, or
      - the resulting Unix time is outside TS_MIN … TS_MAX
        (indicates the node clock was not synced at boot).
    """
    try:
        # Strip trailing Z and parse as UTC
        ts = datetime.strptime(
            raw_t.rstrip("Z"), "%Y-%m-%dT%H:%M:%S.%f"
        ).replace(tzinfo=__import__("datetime").timezone.utc).timestamp()
    except (ValueError, AttributeError):
        now  = time.monotonic()
        last = _last_clock_warn.get(node_id, 0.0)
        if now - last >= _clock_warn_interval:
            _last_clock_warn[node_id] = now
            print(f"[{node_id}] WARNING: cannot parse timestamp {raw_t!r} — packet dropped.")
        return None

    if not (TS_MIN <= ts <= TS_MAX):
        now  = time.monotonic()
        last = _last_clock_warn.get(node_id, 0.0)
        if now - last >= _clock_warn_interval:
            _last_clock_warn[node_id] = now
            try:
                date_str = datetime.utcfromtimestamp(int(ts)).strftime("%Y-%m-%d")
            except Exception:
                date_str = "?"
            print(f"[{node_id}] WARNING: implausible timestamp {raw_t!r} "
                  f"({date_str}) — clock not synced, packet dropped.")
        return None

    return ts


def normalise_sensor_timestamps(data: dict, node_id: str) -> bool:
    """
    Parse ISO 8601 timestamp strings in-place to Unix seconds (float).
    Returns False if ANY sensor timestamp is invalid (whole packet dropped).
    Format: a=[[t,x,y,z],...],  i=[t,x,y,z],  T=[t,val]
    where t is "YYYY-MM-DDTHH:MM:SS.ffffffZ".
    """
    if "a" in data:
        for sample in data["a"]:
            ts = parse_iso_timestamp(str(sample[0]), node_id)
            if ts is None:
                return False
            sample[0] = ts

    if "i" in data:
        ts = parse_iso_timestamp(str(data["i"][0]), node_id)
        if ts is None:
            return False
        data["i"][0] = ts

    if "T" in data:
        ts = parse_iso_timestamp(str(data["T"][0]), node_id)
        if ts is None:
            return False
        data["T"][0] = ts

    return True


# -------------------------------------------------------------------
# Per-node encoding state
# -------------------------------------------------------------------

# state[node_id] = {
#   "accel":  { "ts_us": int, "xyz_prev": [int,int,int] },
#   "inclin": { "ts_us": int, "xyz_prev": [int,int,int] },
#   "temp":   { "ts_us": int, "val_prev": int           },
#   "file_hour": str | None,
#   "is_first":  bool,
# }

node_state:  dict         = {}
data_buffer: queue.Queue  = queue.Queue()


def _fresh_state() -> dict:
    return {
        "accel":  {"ts_us": 0, "xyz_prev": [0, 0, 0]},
        "inclin": {"ts_us": 0, "xyz_prev": [0, 0, 0]},
        "temp":   {"ts_us": 0, "val_prev": 0},
        "file_hour":      None,
        "is_first":       True,
        "header_written": False,
    }


def get_hourly_filepath(node_id: str) -> tuple[str, str]:
    hour_str = datetime.now().strftime("%Y%m%d_%H")
    return hour_str, os.path.join(DATA_DIR, f"data_{node_id}_{hour_str}.bin")


# -------------------------------------------------------------------
# Overflow / clip guards
# -------------------------------------------------------------------

def needs_absolute_record(data: dict, state: dict) -> tuple[bool, str]:
    """Return (True, reason) if any sensor would overflow delta_ts or clip int16."""

    if "a" in data and len(data["a"]) > 0:
        ts_s = float(data["a"][0][0])
        gap  = ts_s - state["accel"]["ts_us"] / TS_SCALE
        if abs(gap) > MAX_DELTA_S:
            return True, f"accel timestamp gap {gap:.1f} s"
        # Check all samples — prev advances through the burst so each sample
        # is compared against the one before it, exactly as encode_delta_record does.
        prev = state["accel"]["xyz_prev"]
        for s in data["a"]:
            xi = int(float(s[1]) * ACCEL_SCALE)
            yi = int(float(s[2]) * ACCEL_SCALE)
            zi = int(float(s[3]) * ACCEL_SCALE)
            for d, name in ((xi-prev[0],"x"),(yi-prev[1],"y"),(zi-prev[2],"z")):
                if abs(d) >= INT16_MAX:
                    return True, f"accel {name} delta {d} would clip int16"
            prev = [xi, yi, zi]

    if "i" in data:
        ts_s = float(data["i"][0])
        gap  = ts_s - state["inclin"]["ts_us"] / TS_SCALE
        if abs(gap) > MAX_DELTA_S:
            return True, f"inclin timestamp gap {gap:.1f} s"
        prev = state["inclin"]["xyz_prev"]
        for idx, name in enumerate(("roll", "pitch")):
            d = int(float(data["i"][idx + 1]) * INCLIN_SCALE) - prev[idx]
            if abs(d) >= INT16_MAX:
                return True, f"inclin {name} delta {d} would clip int16"
        # yaw is int32 — no clip guard needed

    if "T" in data:
        ts_s      = float(data["T"][0])
        ts_us_new = int(ts_s * TS_SCALE)
        # If timestamp hasn't changed, this reading will be skipped in the
        # encoder — no need to check for overflow on a value that won't be written.
        if ts_us_new != state["temp"]["ts_us"]:
            gap = ts_s - state["temp"]["ts_us"] / TS_SCALE
            if abs(gap) > MAX_DELTA_S:
                return True, f"temp timestamp gap {gap:.1f} s"
            d = int(float(data["T"][1]) * TEMP_SCALE) - state["temp"]["val_prev"]
            if abs(d) >= INT16_MAX:
                return True, f"temp delta {d} would clip int16"

    return False, ""


# -------------------------------------------------------------------
# Timestamp encoding helpers
# -------------------------------------------------------------------

def _abs_ts_bytes(ts_s: float, ss: dict) -> bytes:
    """Encode absolute timestamp, update sensor-state ss."""
    ts_us = int(ts_s * TS_SCALE)
    ss["ts_us"] = ts_us
    return struct.pack("<q", ts_us)


def _delta_ts_bytes(ts_s: float, ss: dict) -> bytes:
    """Encode simple delta timestamp (current - previous), update sensor-state ss."""
    ts_us    = int(ts_s * TS_SCALE)
    delta_us = ts_us - ss["ts_us"]
    ss["ts_us"] = ts_us
    return struct.pack("<i", delta_us)


# -------------------------------------------------------------------
# ABSOLUTE record encoder
# -------------------------------------------------------------------

def encode_first_record(data: dict, state: dict) -> bytes:
    """Encode an ABSOLUTE record. All values and timestamps stored as integers."""
    header = 0
    body   = bytearray()

    if "a" in data and len(data["a"]) > 0:
        header |= 0x01
        samples = data["a"]
        body   += struct.pack("<B", len(samples))
        for s in samples:
            xi = int(float(s[1]) * ACCEL_SCALE)
            yi = int(float(s[2]) * ACCEL_SCALE)
            zi = int(float(s[3]) * ACCEL_SCALE)
            body += _abs_ts_bytes(float(s[0]), state["accel"])
            body += struct.pack("<iii", xi, yi, zi)
        last = samples[-1]
        state["accel"]["xyz_prev"] = [
            int(float(last[1]) * ACCEL_SCALE),
            int(float(last[2]) * ACCEL_SCALE),
            int(float(last[3]) * ACCEL_SCALE),
        ]

    if "i" in data:
        header |= 0x02
        iv = data["i"]
        ri = int(float(iv[1]) * INCLIN_SCALE)
        pi = int(float(iv[2]) * INCLIN_SCALE)
        yi = int(float(iv[3]) * INCLIN_SCALE)
        body += _abs_ts_bytes(float(iv[0]), state["inclin"])
        body += struct.pack("<iii", ri, pi, yi)
        state["inclin"]["xyz_prev"] = [ri, pi, yi]

    if "T" in data:
        header |= 0x04
        tv = data["T"]
        vi = int(float(tv[1]) * TEMP_SCALE)
        body += _abs_ts_bytes(float(tv[0]), state["temp"])
        body += struct.pack("<i", vi)
        state["temp"]["val_prev"] = vi

    return struct.pack("<BB", 0xFF, header) + bytes(body)


# -------------------------------------------------------------------
# DELTA record encoder
# -------------------------------------------------------------------

def _pack_delta(value: int, nbytes: int = 2) -> bytes:
    """Clamp and pack a signed delta as int16."""
    return struct.pack("<h", max(-32768, min(32767, value)))


def encode_delta_record(data: dict, state: dict) -> bytes:
    """
    Encode a DELTA record.
    Each sensor field is preceded by a 'changed' bitmask byte:
      bit0 = timestamp delta-of-delta present
      bit1 = first value axis (x/roll/val) changed
      bit2 = second value axis (y/pitch) changed   [accel/inclin]
      bit3 = third value axis (z/yaw) changed      [accel/inclin]
    Only fields whose bit is set are written; zero-delta fields are null/omitted.
    """
    header = 0
    body   = bytearray()

    # ── Accelerometer ─────────────────────────────────────────────
    if "a" in data and len(data["a"]) > 0:
        header |= 0x01
        samples = data["a"]
        body   += struct.pack("<B", len(samples))
        prev    = state["accel"]["xyz_prev"]
        ss      = state["accel"]
        for s in samples:
            xi = int(float(s[1]) * ACCEL_SCALE)
            yi = int(float(s[2]) * ACCEL_SCALE)
            zi = int(float(s[3]) * ACCEL_SCALE)
            dx, dy, dz = xi - prev[0], yi - prev[1], zi - prev[2]

            # Compute delta_ts (simple difference)
            ts_us_new = int(float(s[0]) * TS_SCALE)
            delta_us  = ts_us_new - ss["ts_us"]

            changed = 0
            if delta_us != 0: changed |= 0x01
            if dx       != 0: changed |= 0x02
            if dy       != 0: changed |= 0x04
            if dz       != 0: changed |= 0x08

            body += struct.pack("<B", changed)
            if changed & 0x01: body += struct.pack("<i", delta_us)
            if changed & 0x02: body += _pack_delta(dx)
            if changed & 0x04: body += _pack_delta(dy)
            if changed & 0x08: body += _pack_delta(dz)

            ss["ts_us"] = ts_us_new
            prev = [xi, yi, zi]
        state["accel"]["xyz_prev"] = prev

    # ── Inclinometer ──────────────────────────────────────────────
    if "i" in data:
        header |= 0x02
        iv   = data["i"]
        ss   = state["inclin"]
        ri   = int(float(iv[1]) * INCLIN_SCALE)
        pi   = int(float(iv[2]) * INCLIN_SCALE)
        yi   = int(float(iv[3]) * INCLIN_SCALE)
        prev = ss["xyz_prev"]
        dr, dp, dy = ri - prev[0], pi - prev[1], yi - prev[2]

        ts_us_new = int(float(iv[0]) * TS_SCALE)
        delta_us  = ts_us_new - ss["ts_us"]

        changed = 0
        if delta_us != 0: changed |= 0x01
        if dr       != 0: changed |= 0x02
        if dp       != 0: changed |= 0x04
        if dy       != 0: changed |= 0x08

        body += struct.pack("<B", changed)
        if changed & 0x01: body += struct.pack("<i", delta_us)
        if changed & 0x02: body += _pack_delta(dr)
        if changed & 0x04: body += _pack_delta(dp)
        if changed & 0x08: body += _pack_delta(dy)

        ss["ts_us"]    = ts_us_new
        ss["xyz_prev"] = [ri, pi, yi]

    # ── Temperature ───────────────────────────────────────────────
    if "T" in data:
        tv        = data["T"]
        ss        = state["temp"]
        vi        = int(float(tv[1]) * TEMP_SCALE)
        ts_us_new = int(float(tv[0]) * TS_SCALE)
        delta_us = ts_us_new - ss["ts_us"]
        dt       = vi - ss["val_prev"]

        # Skip entirely if frozen (same timestamp and value as last write)
        is_frozen = (ts_us_new == ss["ts_us"] and vi == ss["val_prev"])
        ss["ts_us"] = ts_us_new

        if not is_frozen:
            header |= 0x04
            changed = 0
            if delta_us != 0: changed |= 0x01
            if dt       != 0: changed |= 0x02

            body += struct.pack("<B", changed)
            if changed & 0x01: body += struct.pack("<i", delta_us)
            if changed & 0x02: body += _pack_delta(dt)

            ss["val_prev"] = vi

    return struct.pack("<B", header) + bytes(body)


# -------------------------------------------------------------------
# Write record
# -------------------------------------------------------------------

def compress_and_replace(node_id: str, bin_path: str):
    """
    Compress a closed .bin file to .zst (single Zstd frame, best ratio)
    and delete the original.  Called only after the file is fully written.
    """
    zst_path = bin_path[:-4] + ".zst"   # strip ".bin", add ".zst"
    try:
        # write_size=True embeds the content size in the frame header so
        # decompressors can use dctx.decompress() without needing stream_reader.
        file_size = os.path.getsize(bin_path)
        cctx = zstd.ZstdCompressor(level=ZSTD_LEVEL, write_content_size=True)
        with open(bin_path, "rb") as f_in, open(zst_path, "wb") as f_out:
            cctx.copy_stream(f_in, f_out, size=file_size)
        original_size   = os.path.getsize(bin_path)
        compressed_size = os.path.getsize(zst_path)
        ratio = compressed_size / original_size * 100 if original_size else 0
        os.remove(bin_path)
        print(f"[{node_id}] Compressed {os.path.basename(bin_path)} → "
              f"{os.path.basename(zst_path)} "
              f"({original_size:,} → {compressed_size:,} bytes, {ratio:.1f}%)")
    except Exception as e:
        print(f"[{node_id}] WARNING: compression failed for {bin_path}: {e}")
        # Leave .bin intact so data is not lost


def write_record(node_id: str, data: dict):
    hour_str, filepath = get_hourly_filepath(node_id)

    if node_id not in node_state:
        node_state[node_id] = _fresh_state()
    state = node_state[node_id]

    if state["file_hour"] != hour_str:
        # Hour boundary: compress the just-closed .bin file, then start fresh
        if state["file_hour"] is not None:
            old_bin = os.path.join(
                DATA_DIR, f"data_{node_id}_{state['file_hour']}.bin")
            if os.path.exists(old_bin):
                compress_and_replace(node_id, old_bin)
        state["file_hour"]      = hour_str
        state["is_first"]       = True
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

    # Append directly to .bin — no buffering needed
    with open(filepath, "ab") as f:
        if not state["header_written"]:
            f.write(struct.pack("<B", FILE_FORMAT_VERSION))
            state["header_written"] = True
        f.write(record)


# -------------------------------------------------------------------
# Consumer thread
# -------------------------------------------------------------------

def consumer_worker():
    while True:
        node_id, data = data_buffer.get()
        try:
            write_record(node_id, data)
        except Exception as e:
            print(f"Error writing record for {node_id}: {e}")
        finally:
            data_buffer.task_done()


consumer_thread = threading.Thread(target=consumer_worker, daemon=True)
consumer_thread.start()

# -------------------------------------------------------------------
# Additional MQTT handling for status messages and ACKs
# -------------------------------------------------------------------

# Build a UTC ISO timestamp for ACK bookkeeping
def now_iso() -> str:
    return datetime.utcnow().isoformat() + "Z"


# Process node status messages and update backend config/runtime state.
def handle_status_message(topic: str, payload_bytes: bytes) -> None:
    try:
        serial = serial_from_topic(topic)
        register_serial(serial)

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

        # Full config ACK from the ESP32
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

        # State-only ACK, e.g. { "state": "recording" }
        update_accelerometer_runtime_state(
            node_id=node["node_id"],
            current_state=current_state,
            acked_at=acked_at,
        )
        print(f"[status] Updated runtime state for {serial}: {current_state}")

    except Exception as e:
        print(f"Failed to process status message on {topic}: {e}")

# -------------------------------------------------------------------
# MQTT callbacks
# -------------------------------------------------------------------

def on_connect(client, userdata, flags, reason_code, properties):
    print("Connected with result code", reason_code)
    client.subscribe(TOPIC)
    client.subscribe(STATUS_TOPIC) # Subscribe to status topic for ACKs


def on_message(client, userdata, msg):
    try:
        if msg.topic.endswith("/status"):  # Handle status messages separately for ACKs and state updates
            handle_status_message(msg.topic, msg.payload)
            return

        if not msg.topic.endswith("/data"):
            return
        
        topic_parts = msg.topic.split("/")
        node_id = topic_parts[1]
        register_serial(node_id)

        # ── Raw backup — written FIRST, before any validation or processing ──
        # Captures exactly what the sensor sent, including packets that will
        # be dropped below (bad clock, missing fields, etc).
        write_raw(node_id, msg.payload)  # raw_backup: verbatim, before processing

        data = json.loads(msg.payload.decode())

        # ---------------------------------------------------------------
        # Fault logging
        # ---------------------------------------------------------------
        # The ESP32 may include an "f" field with a list of fault codes.
        # Example: "f": [1, 4]
        #
        # We log each code as its own row in faults.db using backend
        # receive time, since packet field "t" may not be a true UTC time.
        # ---------------------------------------------------------------
        fault_codes = data.get("f", [])
        mqtt_ts = data.get("t")

        if isinstance(fault_codes, list) and fault_codes:
            if mqtt_ts is None:
                raise ValueError("Fault packet missing timestamp 't'")

            #log_fault_codes(
                serial_number=node_id,
                fault_codes=fault_codes,
                mqtt_ts=mqtt_ts,
            #)

        # Keep your existing timestamp normalization for sensor payloads.
        if not normalise_sensor_timestamps(data, node_id):
            return

        data_buffer.put((node_id, data))

    except Exception as e:
        print(f"Error processing MQTT message on {msg.topic}: {e}")


# -------------------------------------------------------------------
# Main
# -------------------------------------------------------------------

client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
client.on_connect = on_connect
client.on_message = on_message

client.connect(BROKER_IP, PORT, 60)
try:
    client.loop_forever()
finally:
    raw_backup_close_all()