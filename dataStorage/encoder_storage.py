import os
import struct
from datetime import datetime, UTC
import time
import queue
import threading
import gzip
import math

DATA_DIR = "/mnt/ssd/data"
SSD_MOUNT = "/mnt/ssd"

# SSD state tracking — re-evaluated on every write so mid-run unmounts
# and re-mounts are detected automatically.
_ssd_ok = False
_ssd_last_warn = 0.0
_SSD_WARN_INTERVAL = 30.0

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

# Queue setting used by the listener/consumer boundary.
QUEUE_MAX_SIZE = 1000

# -------------------------------------------------------------------
# Gzip compression settings
# -------------------------------------------------------------------
GZIP_LEVEL = 4

# -------------------------------------------------------------------
# Binary encoding settings
# -------------------------------------------------------------------
TEMP_SCALE = 100        # 0.01 °C  -> int
ACCEL_SCALE = 10000     # 0.0001 g -> int
INCLIN_SCALE = 10000    # 0.0001 ° -> int
TS_SCALE = 1_000_000    # seconds  -> µs

FILE_FORMAT_VERSION = 3

MAX_DELTA_S = 60.0
ABSOLUTE_RECORD_INTERVAL_S = 60.0
INT16_MAX = 32767

TS_MIN = 1_577_836_800.0   # 2020-01-01
TS_MAX = 4_102_444_800.0   # 2100-01-01
_clock_warn_interval = 10.0
_last_clock_warn: dict[str, float] = {}

node_state: dict = {}
data_buffer: queue.Queue = queue.Queue(maxsize=QUEUE_MAX_SIZE)
consumer_thread: threading.Thread | None = None


def _check_ssd() -> bool:
    """Return True if the SSD is mounted and the data directory is writable."""
    global _ssd_ok, _ssd_last_warn
    mounted = os.path.isdir(SSD_MOUNT) and os.path.ismount(SSD_MOUNT)
    if mounted:
        try:
            os.makedirs(DATA_DIR, exist_ok=True)
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
    _warn_ssd("SSD not mounted at /mnt/ssd — sensor data will not be saved.")
    _ssd_ok = False
    return False


def _ssd_ok_reset():
    global _ssd_ok
    _ssd_ok = False


def _warn_ssd(msg: str):
    global _ssd_last_warn
    now = time.monotonic()
    if now - _ssd_last_warn >= _SSD_WARN_INTERVAL:
        _ssd_last_warn = now
        print(f"[SSD] WARNING: {msg}")


# Perform initial SSD check at import/startup.
_check_ssd()


def parse_iso_timestamp(raw_t: str, node_id: str) -> float | None:
    """Parse an ISO 8601 UTC timestamp string to Unix seconds (float)."""
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
    """Parse ISO 8601 timestamps in-place to Unix seconds (float)."""
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


def _fresh_state() -> dict:
    return {
        "accel": {"ts_us": 0, "xyz_prev": [0, 0, 0]},
        "inclin": {"ts_us": 0, "xyz_prev": [0, 0, 0]},
        "temp": {"ts_us": 0, "val_prev": 0},
        "file_hour": None,
        "is_first": True,
        "header_written": False,
        "last_absolute_record_ts_us": 0,
    }


def get_hourly_filepath(node_id: str) -> tuple[str, str]:
    hour_str = datetime.now().strftime("%Y%m%d_%H")
    return hour_str, os.path.join(DATA_DIR, f"data_{node_id}_{hour_str}.bin")


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


def _abs_ts_bytes(ts_s: float, ss: dict) -> bytes:
    ts_us = int(ts_s * TS_SCALE)
    ss["ts_us"] = ts_us
    return struct.pack("<q", ts_us)


def _pack_delta(value: int) -> bytes:
    return struct.pack("<h", max(-32768, min(32767, value)))


def _read_exact_or_raise(f, n: int, label: str = "") -> bytes:
    data = f.read(n)
    if len(data) != n:
        raise EOFError(f"EOF reading {label!r}: got {len(data)}/{n} bytes")
    return data


def _skip_v3_changed_samples(f, n: int, label_prefix: str):
    for _ in range(n):
        changed = _read_exact_or_raise(f, 1, f"{label_prefix} changed")[0]
        if changed & CHANGED_TS:
            _read_exact_or_raise(f, 4, f"{label_prefix} delta_ts")
        if changed & CHANGED_X:
            _read_exact_or_raise(f, 2, f"{label_prefix} dx")
        if changed & CHANGED_Y:
            _read_exact_or_raise(f, 2, f"{label_prefix} dy")
        if changed & CHANGED_Z:
            _read_exact_or_raise(f, 2, f"{label_prefix} dz")


def _recover_active_hourly_file(node_id: str, filepath: str) -> bool:
    """Prepare an existing active-hour file for safe append after restart.

    Returns True when the file already contains the version header and should be
    appended to without writing another FILE_FORMAT_VERSION byte.
    Truncates any incomplete trailing record left by an unexpected stop.
    """
    if not os.path.exists(filepath):
        return False

    try:
        size = os.path.getsize(filepath)
    except OSError as e:
        print(f"[{node_id}] WARNING: cannot inspect existing file {filepath}: {e}")
        return False

    if size == 0:
        return False

    try:
        with open(filepath, "r+b") as f:
            version_raw = f.read(1)
            if not version_raw:
                return False

            if version_raw[0] != FILE_FORMAT_VERSION:
                print(
                    f"[{node_id}] WARNING: existing file {os.path.basename(filepath)} "
                    f"starts with version {version_raw[0]}, expected {FILE_FORMAT_VERSION}; "
                    f"appending without rewriting header."
                )
                return True

            last_good_offset = f.tell()

            while True:
                record_start = f.tell()
                marker = f.read(1)
                if not marker:
                    break

                try:
                    if marker[0] == 0xFF:
                        header = _read_exact_or_raise(f, 1, "abs header")[0]
                        if header & 0x01:
                            n = _read_exact_or_raise(f, 1, "accel n")[0]
                            _read_exact_or_raise(f, n * (8 + 12), "accel abs payload")
                        if header & 0x02:
                            n = _read_exact_or_raise(f, 1, "inclin n")[0]
                            _read_exact_or_raise(f, n * (8 + 12), "inclin abs payload")
                        if header & 0x04:
                            _read_exact_or_raise(f, 8 + 4, "temp abs payload")
                    else:
                        header = marker[0]
                        if header & 0x01:
                            n = _read_exact_or_raise(f, 1, "accel n")[0]
                            _skip_v3_changed_samples(f, n, "accel")
                        if header & 0x02:
                            n = _read_exact_or_raise(f, 1, "inclin n")[0]
                            _skip_v3_changed_samples(f, n, "inclin")
                        if header & 0x04:
                            changed = _read_exact_or_raise(f, 1, "temp changed")[0]
                            if changed & CHANGED_TS:
                                _read_exact_or_raise(f, 4, "temp delta_ts")
                            if changed & CHANGED_X:
                                _read_exact_or_raise(f, 2, "temp dval")
                except EOFError:
                    f.truncate(record_start)
                    print(
                        f"[{node_id}] Recovered active file {os.path.basename(filepath)}: "
                        f"truncated incomplete tail from {size} to {record_start} bytes."
                    )
                    return True

                last_good_offset = f.tell()

            if last_good_offset < size:
                f.truncate(last_good_offset)
                print(
                    f"[{node_id}] Recovered active file {os.path.basename(filepath)}: "
                    f"trimmed trailing bytes from {size} to {last_good_offset}."
                )

    except OSError as e:
        print(f"[{node_id}] WARNING: failed to recover existing file {filepath}: {e}")

    return True


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
            if hx:
                prev[0] = xi
            if hy:
                prev[1] = yi
            if hz:
                prev[2] = zi
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
            if hr:
                prev[0] = ri
            if hp:
                prev[1] = pi
            if hy:
                prev[2] = yi
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
                changed |= CHANGED_TS

            if nan_x:
                dx = 0
                changed |= CHANGED_NAN_X
            else:
                xi = int(float(s[1]) * ACCEL_SCALE)
                dx = _apply_null_threshold(xi - prev[0])
                if dx != 0:
                    changed |= CHANGED_X

            if nan_y:
                dy = 0
                changed |= CHANGED_NAN_Y
            else:
                yi = int(float(s[2]) * ACCEL_SCALE)
                dy = _apply_null_threshold(yi - prev[1])
                if dy != 0:
                    changed |= CHANGED_Y

            if nan_z:
                dz = 0
                changed |= CHANGED_NAN_Z
            else:
                zi = int(float(s[3]) * ACCEL_SCALE)
                dz = _apply_null_threshold(zi - prev[2])
                if dz != 0:
                    changed |= CHANGED_Z

            body += struct.pack("<B", changed)
            if changed & CHANGED_TS:
                body += struct.pack("<i", delta_us)
            if changed & CHANGED_X:
                body += _pack_delta(dx)
            if changed & CHANGED_Y:
                body += _pack_delta(dy)
            if changed & CHANGED_Z:
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
                changed |= CHANGED_TS

            if nan_r:
                dr = 0
                changed |= CHANGED_NAN_X
            else:
                ri = int(float(s[1]) * INCLIN_SCALE)
                dr = _apply_null_threshold(ri - prev[0])
                if dr != 0:
                    changed |= CHANGED_X

            if nan_p:
                dp = 0
                changed |= CHANGED_NAN_Y
            else:
                pi = int(float(s[2]) * INCLIN_SCALE)
                dp = _apply_null_threshold(pi - prev[1])
                if dp != 0:
                    changed |= CHANGED_Y

            if nan_y:
                dy = 0
                changed |= CHANGED_NAN_Z
            else:
                yi = int(float(s[3]) * INCLIN_SCALE)
                dy = _apply_null_threshold(yi - prev[2])
                if dy != 0:
                    changed |= CHANGED_Z

            body += struct.pack("<B", changed)
            if changed & CHANGED_TS:
                body += struct.pack("<i", delta_us)
            if changed & CHANGED_X:
                body += _pack_delta(dr)
            if changed & CHANGED_Y:
                body += _pack_delta(dp)
            if changed & CHANGED_Z:
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
            changed |= CHANGED_TS
        if val_is_nan:
            changed |= CHANGED_NAN_TEMP
            dt = 0
        else:
            vi = int(float(tv[1]) * TEMP_SCALE)
            dt = _apply_null_threshold(vi - ss["val_prev"])
            if dt != 0:
                changed |= CHANGED_X

        is_frozen = changed == 0
        ss["ts_us"] = ts_us_new

        if not is_frozen:
            header |= 0x04
            body += struct.pack("<B", changed)
            if changed & CHANGED_TS:
                body += struct.pack("<i", delta_us)
            if changed & CHANGED_X:
                body += _pack_delta(dt)
            if not val_is_nan:
                ss["val_prev"] += dt

    return struct.pack("<B", header) + bytes(body)


def _packet_max_ts_us(data: dict) -> int:
    max_ts_us = 0
    if "a" in data and len(data["a"]) > 0:
        max_ts_us = max(max_ts_us, max(int(float(s[0]) * TS_SCALE) for s in data["a"]))
    if "i" in data and len(data["i"]) > 0:
        max_ts_us = max(max_ts_us, max(int(float(s[0]) * TS_SCALE) for s in data["i"]))
    if "T" in data and len(data["T"]) > 0:
        max_ts_us = max(max_ts_us, int(float(data["T"][0]) * TS_SCALE))
    return max_ts_us


def compress_and_replace(node_id: str, bin_path: str):
    if not _check_ssd():
        print(f"[{node_id}] Skipping compression of {os.path.basename(bin_path)} — SSD unavailable")
        return
    gz_path = bin_path + ".gz"
    try:
        original_size = os.path.getsize(bin_path)
        with open(bin_path, "rb") as f_in, gzip.open(gz_path, "wb", compresslevel=GZIP_LEVEL) as f_out:
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
    if not _check_ssd():
        return

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
        state["last_absolute_record_ts_us"] = 0

        if os.path.exists(filepath):
            state["header_written"] = _recover_active_hourly_file(node_id, filepath)
            print(f"[{node_id}] Resuming hourly file: {filepath} (next record forced ABSOLUTE)")
        else:
            print(f"[{node_id}] New hourly file: {filepath}")

    if not state["is_first"]:
        force_abs, reason = needs_absolute_record(data, state)
        if not force_abs:
            packet_max_ts_us = _packet_max_ts_us(data)
            last_abs_ts_us = state.get("last_absolute_record_ts_us", 0)
            if (
                packet_max_ts_us
                and last_abs_ts_us
                and (packet_max_ts_us - last_abs_ts_us) >= int(ABSOLUTE_RECORD_INTERVAL_S * TS_SCALE)
            ):
                force_abs = True
                reason = f"absolute refresh interval {ABSOLUTE_RECORD_INTERVAL_S:.0f} s reached"
        if force_abs:
            print(f"[{node_id}] Forcing ABSOLUTE record: {reason}")
            state["is_first"] = True

    if state["is_first"]:
        record = encode_first_record(data, state)
        state["is_first"] = False
        state["last_absolute_record_ts_us"] = _packet_max_ts_us(data)
    else:
        record = encode_delta_record(data, state)

    try:
        with open(filepath, "ab") as f:
            if not state["header_written"]:
                f.write(struct.pack("<B", FILE_FORMAT_VERSION))
                state["header_written"] = True
            f.write(record)
    except OSError as e:
        state["header_written"] = False
        state["is_first"] = True
        _ssd_ok_reset()
        _warn_ssd(f"write failed for {node_id}: {e}")


def consumer_worker():
    """Background worker that writes queued sensor packets to disk."""
    _SSD_CHECK_INTERVAL = 60.0
    last_ssd_check = time.monotonic()

    while True:
        try:
            node_id, data = data_buffer.get(timeout=_SSD_CHECK_INTERVAL)
        except queue.Empty:
            _check_ssd()
            last_ssd_check = time.monotonic()
            continue

        try:
            write_record(node_id, data)
        except Exception as e:
            print(f"Error writing record for {node_id}: {e}")
        finally:
            data_buffer.task_done()

        now = time.monotonic()
        if now - last_ssd_check >= _SSD_CHECK_INTERVAL:
            _check_ssd()
            last_ssd_check = now


def start_consumer_thread() -> threading.Thread:
    """Start the background consumer once and return the thread."""
    global consumer_thread
    if consumer_thread is None or not consumer_thread.is_alive():
        consumer_thread = threading.Thread(target=consumer_worker, daemon=True)
        consumer_thread.start()
    return consumer_thread


def enqueue_packet(node_id: str, data: dict) -> bool:
    """Queue a normalized packet for asynchronous binary storage."""
    try:
        data_buffer.put_nowait((node_id, data))
        return True
    except queue.Full:
        print(
            f"[{node_id}] WARNING: queue full ({QUEUE_MAX_SIZE} items) — "
            f"packet dropped. Check if consumer is stalled (disk full/unmounted?)."
        )
        return False
