from __future__ import annotations

import gzip
import io
import struct
from contextlib import ExitStack, contextmanager
from typing import Dict, Generator, Any

TEMP_SCALE = 100
ACCEL_SCALE = 10000
INCLIN_SCALE = 10000
TS_SCALE = 1_000_000

FLAG_ACCEL = 0x01
FLAG_INCLIN = 0x02
FLAG_TEMP = 0x04
SENTINEL = 0xFF

FORMAT_V1 = 1
FORMAT_V2 = 2
FORMAT_V3 = 3

INT32_NAN_SENTINEL = -2147483648


def read_bytes(f, n, label=""):
    d = f.read(n)
    if len(d) < n:
        raise EOFError(f"EOF reading {label!r}: got {len(d)}/{n} bytes")
    return d


def read_fmt(f, fmt, label=""):
    raw = read_bytes(f, struct.calcsize(fmt), label)
    return struct.unpack(fmt, raw)


def _fresh_sensor_state():
    return {
        "ts_us": 0,
        "ts_delta_prev": 0,
        "xyz_prev": [0, 0, 0],
        "val_prev": 0,
    }


def _fresh_decode_state():
    return {
        "accel": _fresh_sensor_state(),
        "inclin": _fresh_sensor_state(),
        "temp": _fresh_sensor_state(),
    }


def _reconstruct_abs_ts(f, ss: dict) -> int:
    (ts_us,) = read_fmt(f, "<q", "abs timestamp")
    ss["ts_us"] = ts_us
    return ts_us


def _decode_abs_value(raw: int, scale: int):
    if raw == INT32_NAN_SENTINEL:
        return None
    return raw / scale


def _v1_reconstruct_ts(f, ss: dict) -> int:
    (dod,) = read_fmt(f, "<i", "v1 dod_ts")
    delta_us = ss.get("ts_delta_prev", 0) + dod
    ts_us = ss["ts_us"] + delta_us
    ss["ts_us"] = ts_us
    ss["ts_delta_prev"] = delta_us
    return ts_us


def _decode_accel_abs(f, state):
    (n,) = read_fmt(f, "<B", "accel n")
    out = []
    prev = list(state["accel"]["xyz_prev"])

    for _ in range(n):
        ts_us = _reconstruct_abs_ts(f, state["accel"])
        x, y, z = read_fmt(f, "<iii", "accel abs xyz")

        x_value = _decode_abs_value(x, ACCEL_SCALE)
        y_value = _decode_abs_value(y, ACCEL_SCALE)
        z_value = _decode_abs_value(z, ACCEL_SCALE)

        if x_value is not None:
            prev[0] = x
        if y_value is not None:
            prev[1] = y
        if z_value is not None:
            prev[2] = z

        out.append(
            (
                ts_us / TS_SCALE,
                x_value,
                y_value,
                z_value,
            )
        )

    state["accel"]["xyz_prev"] = prev
    return out or None


def _decode_accel_delta_v1(f, state):
    (n,) = read_fmt(f, "<B", "accel n")
    out = []
    ss = state["accel"]
    prev = ss["xyz_prev"]

    for _ in range(n):
        ts_us = _v1_reconstruct_ts(f, ss)
        dx, dy, dz = read_fmt(f, "<hhh", "accel delta xyz")
        cur = [prev[0] + dx, prev[1] + dy, prev[2] + dz]

        out.append(
            (
                ts_us / TS_SCALE,
                cur[0] / ACCEL_SCALE,
                cur[1] / ACCEL_SCALE,
                cur[2] / ACCEL_SCALE,
            )
        )

        prev = cur

    ss["xyz_prev"] = prev
    return out or None


def _decode_accel_delta(f, state):
    (n,) = read_fmt(f, "<B", "accel n")
    out = []
    ss = state["accel"]
    prev = ss["xyz_prev"]

    for _ in range(n):
        (changed,) = read_fmt(f, "<B", "accel changed")

        if changed & 0x01:
            (delta_us,) = read_fmt(f, "<i", "accel delta_ts")
            ts_us = ss["ts_us"] + delta_us
            ss["ts_us"] = ts_us
        else:
            ts_us = ss["ts_us"]

        dx = read_fmt(f, "<h", "accel dx")[0] if changed & 0x02 else 0
        dy = read_fmt(f, "<h", "accel dy")[0] if changed & 0x04 else 0
        dz = read_fmt(f, "<h", "accel dz")[0] if changed & 0x08 else 0

        cur = [prev[0] + dx, prev[1] + dy, prev[2] + dz]

        out.append(
            (
                ts_us / TS_SCALE,
                cur[0] / ACCEL_SCALE,
                cur[1] / ACCEL_SCALE,
                cur[2] / ACCEL_SCALE,
            )
        )

        prev = cur

    ss["xyz_prev"] = prev
    return out or None



def _decode_accel_delta_v3(f, state):
    (n,) = read_fmt(f, "<B", "accel n")
    out = []
    ss = state["accel"]
    prev = ss["xyz_prev"]

    for _ in range(n):
        (changed,) = read_fmt(f, "<B", "accel changed")

        if changed & 0x01:
            (delta_us,) = read_fmt(f, "<i", "accel delta_ts")
            ts_us = ss["ts_us"] + delta_us
            ss["ts_us"] = ts_us
        else:
            ts_us = ss["ts_us"]

        if changed & 0x10:
            x = None
        else:
            dx = read_fmt(f, "<h", "accel dx")[0] if changed & 0x02 else 0
            x = prev[0] + dx

        if changed & 0x20:
            y = None
        else:
            dy = read_fmt(f, "<h", "accel dy")[0] if changed & 0x04 else 0
            y = prev[1] + dy

        if changed & 0x40:
            z = None
        else:
            dz = read_fmt(f, "<h", "accel dz")[0] if changed & 0x08 else 0
            z = prev[2] + dz

        out.append(
            (
                ts_us / TS_SCALE,
                None if x is None else x / ACCEL_SCALE,
                None if y is None else y / ACCEL_SCALE,
                None if z is None else z / ACCEL_SCALE,
            )
        )

        prev = [
            prev[0] if x is None else x,
            prev[1] if y is None else y,
            prev[2] if z is None else z,
        ]

    ss["xyz_prev"] = prev
    return out or None


def _decode_inclin_abs(f, state):
    ts_us = _reconstruct_abs_ts(f, state["inclin"])
    roll, pitch, yaw = read_fmt(f, "<iii", "inclin abs")
    state["inclin"]["xyz_prev"] = [roll, pitch, yaw]

    return (
        ts_us / TS_SCALE,
        roll / INCLIN_SCALE,
        pitch / INCLIN_SCALE,
        yaw / INCLIN_SCALE,
    )


def _decode_inclin_abs_v3(f, state):
    (n,) = read_fmt(f, "<B", "inclin n")
    out = []
    prev = list(state["inclin"]["xyz_prev"])

    for _ in range(n):
        ts_us = _reconstruct_abs_ts(f, state["inclin"])
        roll, pitch, yaw = read_fmt(f, "<iii", "inclin abs")

        roll_value = _decode_abs_value(roll, INCLIN_SCALE)
        pitch_value = _decode_abs_value(pitch, INCLIN_SCALE)
        yaw_value = _decode_abs_value(yaw, INCLIN_SCALE)

        if roll_value is not None:
            prev[0] = roll
        if pitch_value is not None:
            prev[1] = pitch
        if yaw_value is not None:
            prev[2] = yaw

        out.append(
            (
                ts_us / TS_SCALE,
                roll_value,
                pitch_value,
                yaw_value,
            )
        )

    state["inclin"]["xyz_prev"] = prev
    return out or None


def _decode_inclin_delta_v1(f, state):
    ss = state["inclin"]
    ts_us = _v1_reconstruct_ts(f, ss)
    dr, dp, dy = read_fmt(f, "<hhh", "inclin delta")
    prev = ss["xyz_prev"]
    cur = [prev[0] + dr, prev[1] + dp, prev[2] + dy]
    ss["xyz_prev"] = cur

    return (
        ts_us / TS_SCALE,
        cur[0] / INCLIN_SCALE,
        cur[1] / INCLIN_SCALE,
        cur[2] / INCLIN_SCALE,
    )


def _decode_inclin_delta(f, state):
    ss = state["inclin"]
    (changed,) = read_fmt(f, "<B", "inclin changed")

    if changed & 0x01:
        (delta_us,) = read_fmt(f, "<i", "inclin delta_ts")
        ts_us = ss["ts_us"] + delta_us
        ss["ts_us"] = ts_us
    else:
        ts_us = ss["ts_us"]

    prev = ss["xyz_prev"]
    dr = read_fmt(f, "<h", "inclin dr")[0] if changed & 0x02 else 0
    dp = read_fmt(f, "<h", "inclin dp")[0] if changed & 0x04 else 0
    dy = read_fmt(f, "<h", "inclin dy")[0] if changed & 0x08 else 0

    cur = [prev[0] + dr, prev[1] + dp, prev[2] + dy]
    ss["xyz_prev"] = cur

    return (
        ts_us / TS_SCALE,
        cur[0] / INCLIN_SCALE,
        cur[1] / INCLIN_SCALE,
        cur[2] / INCLIN_SCALE,
    )



def _decode_inclin_delta_v3(f, state):
    (n,) = read_fmt(f, "<B", "inclin n")
    out = []
    ss = state["inclin"]
    prev = ss["xyz_prev"]

    for _ in range(n):
        (changed,) = read_fmt(f, "<B", "inclin changed")

        if changed & 0x01:
            (delta_us,) = read_fmt(f, "<i", "inclin delta_ts")
            ts_us = ss["ts_us"] + delta_us
            ss["ts_us"] = ts_us
        else:
            ts_us = ss["ts_us"]

        if changed & 0x10:
            roll = None
        else:
            dr = read_fmt(f, "<h", "inclin dr")[0] if changed & 0x02 else 0
            roll = prev[0] + dr

        if changed & 0x20:
            pitch = None
        else:
            dp = read_fmt(f, "<h", "inclin dp")[0] if changed & 0x04 else 0
            pitch = prev[1] + dp

        if changed & 0x40:
            yaw = None
        else:
            dy = read_fmt(f, "<h", "inclin dy")[0] if changed & 0x08 else 0
            yaw = prev[2] + dy

        out.append(
            (
                ts_us / TS_SCALE,
                None if roll is None else roll / INCLIN_SCALE,
                None if pitch is None else pitch / INCLIN_SCALE,
                None if yaw is None else yaw / INCLIN_SCALE,
            )
        )

        prev = [
            prev[0] if roll is None else roll,
            prev[1] if pitch is None else pitch,
            prev[2] if yaw is None else yaw,
        ]

    ss["xyz_prev"] = prev
    return out or None


def _decode_temp_abs(f, state):
    ts_us = _reconstruct_abs_ts(f, state["temp"])
    (val,) = read_fmt(f, "<i", "temp abs")

    value = _decode_abs_value(val, TEMP_SCALE)
    if value is not None:
        state["temp"]["val_prev"] = val

    return (
        ts_us / TS_SCALE,
        value,
    )


def _decode_temp_delta_v1(f, state):
    ss = state["temp"]
    ts_us = _v1_reconstruct_ts(f, ss)
    (dt,) = read_fmt(f, "<h", "temp delta")
    val = ss["val_prev"] + dt
    ss["val_prev"] = val

    return (
        ts_us / TS_SCALE,
        val / TEMP_SCALE,
    )


def _decode_temp_delta(f, state):
    ss = state["temp"]
    (changed,) = read_fmt(f, "<B", "temp changed")

    if changed & 0x01:
        (delta_us,) = read_fmt(f, "<i", "temp delta_ts")
        ts_us = ss["ts_us"] + delta_us
        ss["ts_us"] = ts_us
    else:
        ts_us = ss["ts_us"]

    if changed & 0x02:
        (dt,) = read_fmt(f, "<h", "temp dval")
        ss["val_prev"] = ss["val_prev"] + dt

    return (
        ts_us / TS_SCALE,
        ss["val_prev"] / TEMP_SCALE,
    )


def _decode_temp_delta_v3(f, state):
    ss = state["temp"]
    (changed,) = read_fmt(f, "<B", "temp changed")

    if changed & 0x01:
        (delta_us,) = read_fmt(f, "<i", "temp delta_ts")
        ts_us = ss["ts_us"] + delta_us
        ss["ts_us"] = ts_us
    else:
        ts_us = ss["ts_us"]

    if changed & 0x04:
        return (
            ts_us / TS_SCALE,
            None,
        )

    if changed & 0x02:
        (dt,) = read_fmt(f, "<h", "temp dval")
        ss["val_prev"] = ss["val_prev"] + dt

    return (
        ts_us / TS_SCALE,
        ss["val_prev"] / TEMP_SCALE,
    )


@contextmanager
def open_record_stream(filepath: str):
    """
    Streaming opener for raw sensor files used by plotting.

    Supported:
    - .bin
    - .bin.gz

    This matches the frontend decoder's file support while keeping backend
    RAM usage low by streaming instead of fully decompressing into memory.
    """
    with ExitStack() as stack:
        if filepath.endswith(".gz") or filepath.endswith(".gzip"):
            gzip_file = stack.enter_context(gzip.open(filepath, "rb"))
            buffered = io.BufferedReader(gzip_file)
            yield buffered
        else:
            raw_file = stack.enter_context(open(filepath, "rb"))
            buffered = io.BufferedReader(raw_file)
            yield buffered


def iter_decoded_records_for_export(
    filepath: str,
) -> Generator[Dict[str, Any], None, None]:
    """
    Streaming decoder used by the backend plot endpoints.

    This now matches the frontend decoder behavior more closely:
    - supports .bin and .bin.gz
    - supports FORMAT_V1, FORMAT_V2, and FORMAT_V3
    - tolerates a truncated tail record by stopping cleanly
    - yields one decoded record at a time for low-memory processing

    Export no longer uses this path. Raw export packages the storage files
    directly without decoding.
    """
    state = _fresh_decode_state()

    with open_record_stream(filepath) as f:
        ver = f.read(1)
        if not ver:
            return

        fv = ver[0]
        if fv not in (FORMAT_V1, FORMAT_V2, FORMAT_V3):
            # Legacy file with no explicit version byte.
            f.seek(-1, 1)
            fv = FORMAT_V1

        idx = 0

        while True:
            b = f.read(1)
            if not b:
                break

            sentinel = b[0]

            try:
                if sentinel == SENTINEL:
                    (header,) = read_fmt(f, "<B", "abs header")
                    rec = {
                        "record_index": idx,
                        "record_type": "ABSOLUTE",
                        "accel_samples": None,
                        "inclin": None,
                        "temp": None,
                    }

                    if header & FLAG_ACCEL:
                        rec["accel_samples"] = _decode_accel_abs(f, state)
                    if header & FLAG_INCLIN:
                        if fv == FORMAT_V3:
                            rec["inclin"] = _decode_inclin_abs_v3(f, state)
                        else:
                            rec["inclin"] = _decode_inclin_abs(f, state)
                    if header & FLAG_TEMP:
                        rec["temp"] = _decode_temp_abs(f, state)
                else:
                    header = sentinel
                    rec = {
                        "record_index": idx,
                        "record_type": "DELTA",
                        "accel_samples": None,
                        "inclin": None,
                        "temp": None,
                    }

                    if fv == FORMAT_V1:
                        if header & FLAG_ACCEL:
                            rec["accel_samples"] = _decode_accel_delta_v1(f, state)
                        if header & FLAG_INCLIN:
                            rec["inclin"] = _decode_inclin_delta_v1(f, state)
                        if header & FLAG_TEMP:
                            rec["temp"] = _decode_temp_delta_v1(f, state)
                    elif fv == FORMAT_V2:
                        if header & FLAG_ACCEL:
                            rec["accel_samples"] = _decode_accel_delta(f, state)
                        if header & FLAG_INCLIN:
                            rec["inclin"] = _decode_inclin_delta(f, state)
                        if header & FLAG_TEMP:
                            rec["temp"] = _decode_temp_delta(f, state)
                    else:
                        if header & FLAG_ACCEL:
                            rec["accel_samples"] = _decode_accel_delta_v3(f, state)
                        if header & FLAG_INCLIN:
                            rec["inclin"] = _decode_inclin_delta_v3(f, state)
                        if header & FLAG_TEMP:
                            rec["temp"] = _decode_temp_delta_v3(f, state)

                yield rec
                idx += 1

            except EOFError:
                # Match the frontend decoder behavior:
                # keep all earlier decoded records and stop cleanly if the file
                # ends mid-record, which can happen for active .bin files or
                # interrupted/incomplete files.
                break
            except struct.error:
                break