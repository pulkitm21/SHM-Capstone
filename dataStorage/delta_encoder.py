import json
import os
import struct
from datetime import datetime
import paho.mqtt.client as mqtt
import queue
import threading

from node_registry import register_serial, serial_from_topic

BROKER_IP = "localhost"
PORT = 1883
TOPIC = "wind_turbine/+/data"
DATA_DIR = "/mnt/ssd/data"

os.makedirs(DATA_DIR, exist_ok=True)

# -------------------------------------------------------------------
# Binary format for a single combined record (all fields, per node):
#
# Header byte (1 byte):  bitmask — bit0=accel, bit1=inclin, bit2=temp
# Timestamp delta-of-delta (int32, 4 bytes) — µs resolution
#
# If accel present (bit0):
#   num_samples (uint8, 1 byte)
#   per sample: dx (int16), dy (int16), dz (int16)  ← delta from prev
#
# If inclin present (bit1):
#   d_roll (int16), d_pitch (int16), d_yaw (int16)  ← delta from prev
#
# If temp present (bit2):
#   d_temp (int16)  ← delta from prev, unit = 0.01 °C
#
# First record per file uses absolute values encoded as int32/int32
# (timestamp as raw µs int64, sensor values as int32 with same scale).
# -------------------------------------------------------------------

TEMP_SCALE   = 100      # 0.01 °C resolution  → store as int16
ACCEL_SCALE  = 1000     # 0.001 g  resolution → store as int16
INCLIN_SCALE = 1000     # 0.001 °  resolution → store as int16
TS_SCALE     = 1_000_000  # seconds → µs (stored as int64 on first, int32 delta-of-delta)

# FIX: maximum gap (seconds) before we force an ABSOLUTE record instead of a
# delta.  int32 at µs resolution overflows at 2^31 µs ≈ 2147 s ≈ 35 min.
# Using a conservative 60 s limit keeps dod_ts comfortably within range and
# also handles broker reconnects / NTP steps cleanly.
MAX_DELTA_S = 60.0

# int16 safe headroom — if any scaled delta would reach this, fall back to ABSOLUTE
INT16_MAX = 32767

# Linear buffer: items are (node_id, raw_data_dict)
data_buffer = queue.Queue()

# Per-node state for delta encoding
# state[node_id] = {
#   "ts_prev": float,       # previous timestamp (s)
#   "ts_delta_prev": int,   # previous timestamp delta (µs)
#   "accel_prev": [x,y,z],  # last accel sample
#   "inclin_prev": [r,p,y],
#   "temp_prev": float,
#   "file_hour": str,       # "YYYYMMDD_HH" of the current open file
#   "is_first": bool,       # True until first record written to current file
# }
node_state = {}


def get_hourly_filepath(node_id: str) -> tuple[str, str]:
    """Return (hour_str, filepath) for the current hour."""
    hour_str = datetime.now().strftime("%Y%m%d_%H")
    path = os.path.join(DATA_DIR, f"data_{node_id}_{hour_str}.bin")
    return hour_str, path


def _fresh_state() -> dict:
    return {
        "ts_prev":       0.0,
        "ts_delta_prev": 0,
        "accel_prev":    [0.0, 0.0, 0.0],
        "inclin_prev":   [0.0, 0.0, 0.0],
        "temp_prev":     0.0,
        "file_hour":     None,
        "is_first":      True,
    }


def needs_absolute_record(data: dict, state: dict) -> tuple[bool, str]:
    """
    Return (True, reason) when an ABSOLUTE record must be emitted instead of
    a delta, to prevent int32 dod_ts overflow or int16 sensor-delta clipping.
    """
    # 1. Large timestamp gap → dod_ts would overflow int32
    ts = data["t"]
    gap = ts - state["ts_prev"]
    if abs(gap) > MAX_DELTA_S:
        return True, f"timestamp gap {gap:.1f} s > MAX_DELTA_S {MAX_DELTA_S} s"

    # 2. Any inclin delta would overflow int16
    if "i" in data:
        iv   = [float(v) for v in data["i"]]
        prev = state["inclin_prev"]
        for curr, prv, name in zip(iv, prev, ("roll", "pitch", "yaw")):
            d = int(curr * INCLIN_SCALE) - int(prv * INCLIN_SCALE)
            if abs(d) >= INT16_MAX:
                return True, f"inclin {name} delta {d} would clip int16"

    # 3. Any temp delta would overflow int16
    if "T" in data:
        dt = int(float(data["T"]) * TEMP_SCALE) - int(state["temp_prev"] * TEMP_SCALE)
        if abs(dt) >= INT16_MAX:
            return True, f"temp delta {dt} would clip int16"

    # 4. Any accel delta would overflow int16 (check last sample vs state)
    if "a" in data and len(data["a"]) > 0:
        first = data["a"][0]
        prev  = state["accel_prev"]
        for curr, prv, name in zip(first, prev, ("x", "y", "z")):
            d = int(float(curr) * ACCEL_SCALE) - int(prv * ACCEL_SCALE)
            if abs(d) >= INT16_MAX:
                return True, f"accel {name} delta {d} would clip int16"

    return False, ""


def encode_first_record(data: dict, state: dict) -> bytes:
    """Encode an ABSOLUTE record.  Updates state in-place."""
    ts_us = int(data["t"] * TS_SCALE)
    state["ts_prev"]       = data["t"]
    state["ts_delta_prev"] = 0          # reset dod accumulator on every absolute

    header = 0
    body   = bytearray()

    # Accel — absolute int32 per axis
    if "a" in data and len(data["a"]) > 0:
        header |= 0x01
        samples = data["a"]
        state["accel_prev"] = [float(v) for v in samples[-1]]
        body += struct.pack("<B", len(samples))
        for s in samples:
            body += struct.pack("<iii",
                int(float(s[0]) * ACCEL_SCALE),
                int(float(s[1]) * ACCEL_SCALE),
                int(float(s[2]) * ACCEL_SCALE))

    # Inclin — absolute int32 per axis
    if "i" in data:
        header |= 0x02
        iv = [float(v) for v in data["i"]]
        state["inclin_prev"] = iv
        body += struct.pack("<iii",
            int(iv[0] * INCLIN_SCALE),
            int(iv[1] * INCLIN_SCALE),
            int(iv[2] * INCLIN_SCALE))

    # Temp — absolute int32
    if "T" in data:
        header |= 0x04
        t = float(data["T"])
        state["temp_prev"] = t
        body += struct.pack("<i", int(t * TEMP_SCALE))

    # Layout: sentinel(0xFF) | header(B) | ts_abs(q) | body
    return struct.pack("<BBq", 0xFF, header, ts_us) + bytes(body)


def encode_delta_record(data: dict, state: dict) -> bytes:
    """Encode a DELTA record using delta / delta-of-delta.  Updates state in-place."""
    ts = data["t"]
    ts_us      = int(ts * TS_SCALE)
    ts_prev_us = int(state["ts_prev"] * TS_SCALE)

    delta_us = ts_us - ts_prev_us
    dod_us   = delta_us - state["ts_delta_prev"]

    state["ts_prev"]       = ts
    state["ts_delta_prev"] = delta_us

    header = 0
    body   = bytearray()

    # Accel delta
    if "a" in data and len(data["a"]) > 0:
        header |= 0x01
        samples = data["a"]
        prev    = state["accel_prev"]
        body   += struct.pack("<B", len(samples))
        for s in samples:
            dx = int(float(s[0]) * ACCEL_SCALE) - int(prev[0] * ACCEL_SCALE)
            dy = int(float(s[1]) * ACCEL_SCALE) - int(prev[1] * ACCEL_SCALE)
            dz = int(float(s[2]) * ACCEL_SCALE) - int(prev[2] * ACCEL_SCALE)
            body += struct.pack("<hhh",
                max(-32768, min(32767, dx)),
                max(-32768, min(32767, dy)),
                max(-32768, min(32767, dz)))
            prev = [float(s[0]), float(s[1]), float(s[2])]
        state["accel_prev"] = [float(v) for v in samples[-1]]

    # Inclin delta
    if "i" in data:
        header |= 0x02
        iv   = [float(v) for v in data["i"]]
        prev = state["inclin_prev"]
        dr = int(iv[0] * INCLIN_SCALE) - int(prev[0] * INCLIN_SCALE)
        dp = int(iv[1] * INCLIN_SCALE) - int(prev[1] * INCLIN_SCALE)
        dy = int(iv[2] * INCLIN_SCALE) - int(prev[2] * INCLIN_SCALE)
        body += struct.pack("<hhh",
            max(-32768, min(32767, dr)),
            max(-32768, min(32767, dp)),
            max(-32768, min(32767, dy)))
        state["inclin_prev"] = iv

    # Temp delta
    if "T" in data:
        header |= 0x04
        t    = float(data["T"])
        prev = state["temp_prev"]
        dt   = int(t * TEMP_SCALE) - int(prev * TEMP_SCALE)
        body += struct.pack("<h", max(-32768, min(32767, dt)))
        state["temp_prev"] = t

    # Layout: header(B) | dod_ts(i) | body
    return struct.pack("<Bi", header, dod_us) + bytes(body)


def write_record(node_id: str, data: dict):
    """Encode and append one record to the current hourly file."""
    hour_str, filepath = get_hourly_filepath(node_id)

    # Initialise state for a brand-new node
    if node_id not in node_state:
        node_state[node_id] = _fresh_state()

    state = node_state[node_id]

    # FIX: hour boundary — only set is_first; do NOT wipe the rest of state.
    # The sensor values haven't changed at midnight; wiping accel_prev /
    # inclin_prev / temp_prev would make the first delta in the new file
    # wrong.  We only need a fresh absolute timestamp anchor.
    if state["file_hour"] != hour_str:
        state["file_hour"] = hour_str
        state["is_first"]  = True
        print(f"[{node_id}] New hourly file: {filepath}")

    # FIX: force ABSOLUTE whenever a large gap or pending overflow is detected,
    # even mid-file.  This prevents dod_ts int32 overflow and int16 clipping.
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

    with open(filepath, "ab") as f:
        f.write(record)


# -------------------------------------------------------------------
# Consumer thread: drains the queue and writes records
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
# MQTT callbacks
# -------------------------------------------------------------------

def on_connect(client, userdata, flags, reason_code, properties):
    print("Connected with result code", reason_code)
    client.subscribe(TOPIC)


def on_message(client, userdata, msg):
    try:
        topic_parts = msg.topic.split("/")
        node_id = topic_parts[1]
        register_serial(node_id)
        data    = json.loads(msg.payload.decode())
        data_buffer.put((node_id, data))   # non-blocking enqueue
        print(f"Queued data for serial {node_id} from topic {msg.topic}")
 
    except Exception as e:
        print(f"Failed to process MQTT message on topic {msg.topic}: {e}")


# -------------------------------------------------------------------
# Main
# -------------------------------------------------------------------

client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
client.on_connect = on_connect
client.on_message = on_message

client.connect(BROKER_IP, PORT, 60)
client.loop_forever()