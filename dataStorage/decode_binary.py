"""
decode_binary.py
----------------
Decodes binary files written by multiple_nodes_test.py and prints
records in a human-readable table.  Can also export to CSV.

Usage:
    python decode_binary.py <file.bin>
    python decode_binary.py <file.bin> --csv output.csv
    python decode_binary.py <file.bin> --verbose
"""

import struct
import sys
import os
import argparse
import csv
from datetime import datetime, timezone

# ── Scale factors (must match encoder) ────────────────────────────
TEMP_SCALE   = 100
ACCEL_SCALE  = 1000
INCLIN_SCALE = 1000
TS_SCALE     = 1_000_000   # µs → seconds

# ── Bitmask flags ──────────────────────────────────────────────────
FLAG_ACCEL  = 0x01
FLAG_INCLIN = 0x02
FLAG_TEMP   = 0x04
FIRST_RECORD_SENTINEL = 0xFF


# ──────────────────────────────────────────────────────────────────
# Low-level reader helpers
# ──────────────────────────────────────────────────────────────────

def read_bytes(f, n: int, label: str = "") -> bytes:
    data = f.read(n)
    if len(data) < n:
        raise EOFError(f"Unexpected end of file reading {label} ({len(data)}/{n} bytes)")
    return data


def read_fmt(f, fmt: str, label: str = ""):
    size = struct.calcsize(fmt)
    raw  = read_bytes(f, size, label)
    return struct.unpack(fmt, raw)


# ──────────────────────────────────────────────────────────────────
# Record decoders
# ──────────────────────────────────────────────────────────────────

def decode_first_record(f, state: dict) -> dict:
    """
    Layout: 0xFF (already consumed) | header(B) | ts_abs(q) | body…
    Accel body:  num_samples(B) + num_samples × [x(i) y(i) z(i)]
    Inclin body: r(i) p(i) y(i)
    Temp body:   t(i)
    """
    (header,) = read_fmt(f, "<B", "first-record header")
    (ts_us,)  = read_fmt(f, "<q", "absolute timestamp")

    ts_s = ts_us / TS_SCALE
    state["ts_us"]         = ts_us
    state["ts_delta_prev"] = 0

    record = {
        "timestamp": ts_s,
        "datetime":  _ts_to_str(ts_s),
        "accel_samples": None,
        "inclin": None,
        "temp":   None,
        "record_type": "ABSOLUTE",
    }

    if header & FLAG_ACCEL:
        (n,) = read_fmt(f, "<B", "accel sample count")
        samples = []
        for _ in range(n):
            x, y, z = read_fmt(f, "<iii", "accel abs sample")
            samples.append([x, y, z])  # store raw ints
        state["accel_prev"] = samples[-1] if samples else [0, 0, 0]
        record["accel_samples"] = [(x/ACCEL_SCALE, y/ACCEL_SCALE, z/ACCEL_SCALE) for x,y,z in samples] if samples else None

    if header & FLAG_INCLIN:
        r, p, y = read_fmt(f, "<iii", "inclin abs")
        state["inclin_prev"] = [r, p, y]
        record["inclin"] = (r/INCLIN_SCALE, p/INCLIN_SCALE, y/INCLIN_SCALE)

    if header & FLAG_TEMP:
        (t,) = read_fmt(f, "<i", "temp abs")
        state["temp_prev"] = t
        record["temp"] = t / TEMP_SCALE

    return record


def decode_delta_record(f, state: dict) -> dict:
    """
    Layout: header(B) | dod_ts(i) | body…
    Accel body:  num_samples(B) + num_samples × [dx(h) dy(h) dz(h)]
    Inclin body: dr(h) dp(h) dy(h)
    Temp body:   dt(h)
    """
    (header,) = read_fmt(f, "<B", "delta header")
    (dod_us,) = read_fmt(f, "<i", "delta-of-delta timestamp")

    # Reconstruct timestamp
    delta_us = state["ts_delta_prev"] + dod_us
    ts_us    = state["ts_us"] + delta_us
    ts_s     = ts_us / TS_SCALE

    state["ts_delta_prev"] = delta_us
    state["ts_us"]         = ts_us

    record = {
        "timestamp": ts_s,
        "datetime":  _ts_to_str(ts_s),
        "accel_samples": None,
        "inclin": None,
        "temp":   None,
        "record_type": "DELTA",
    }

    if header & FLAG_ACCEL:
        (n,) = read_fmt(f, "<B", "accel sample count")
        samples = []
        cur = state["accel_prev"][:]
        samples_int = []
        for _ in range(n):
            dx, dy, dz = read_fmt(f, "<hhh", "accel delta sample")
            cur = [cur[0]+dx, cur[1]+dy, cur[2]+dz]
            samples_int.append(cur[:])
        if samples_int:
            state["accel_prev"] = samples_int[-1]
        record["accel_samples"] = [(x/ACCEL_SCALE, y/ACCEL_SCALE, z/ACCEL_SCALE) for x,y,z in samples_int] if samples_int else None

    if header & FLAG_INCLIN:
        dr, dp, dy = read_fmt(f, "<hhh", "inclin delta")
        prev = state["inclin_prev"]
        state["inclin_prev"] = [prev[0]+dr, prev[1]+dp, prev[2]+dy]
        record["inclin"] = tuple(v/INCLIN_SCALE for v in state["inclin_prev"])

    if header & FLAG_TEMP:
        (dt,) = read_fmt(f, "<h", "temp delta")
        state["temp_prev"] = state["temp_prev"] + dt
        record["temp"] = state["temp_prev"] / TEMP_SCALE

    return record


# ──────────────────────────────────────────────────────────────────
# File decoder
# ──────────────────────────────────────────────────────────────────

def decode_file(filepath: str, verbose: bool = False):
    """Decode all records from a binary file.  Returns list of record dicts."""
    records = []
    # State stored as integer scaled units — no float accumulation.
    state = {
        "ts_us":         0,
        "ts_delta_prev": 0,
        "accel_prev":    [0, 0, 0],
        "inclin_prev":   [0, 0, 0],
        "temp_prev":     0,
    }

    filesize = os.path.getsize(filepath)
    bytes_read = 0

    with open(filepath, "rb") as f:
        record_idx = 0
        while True:
            byte = f.read(1)
            if not byte:
                break  # clean EOF

            sentinel = byte[0]
            bytes_read += 1

            try:
                if sentinel == FIRST_RECORD_SENTINEL:
                    rec = decode_first_record(f, state)
                else:
                    # Put the byte back by seeking — but we already consumed it.
                    # The header byte IS the sentinel for delta records.
                    # Re-use it: build a fake 1-byte buffer and pass header directly.
                    rec = _decode_delta_with_header(f, state, sentinel)

                rec["record_index"] = record_idx
                records.append(rec)

                if verbose:
                    _print_record(rec)

                record_idx += 1

            except EOFError as e:
                print(f"\n[WARNING] Truncated record at index {record_idx}: {e}", file=sys.stderr)
                break
            except struct.error as e:
                print(f"\n[ERROR] Struct unpack failed at record {record_idx}: {e}", file=sys.stderr)
                break

    return records


def _decode_delta_with_header(f, state: dict, header: int) -> dict:
    """Delta record where we already consumed the header byte."""
    (dod_us,) = read_fmt(f, "<i", "delta-of-delta timestamp")

    delta_us = state["ts_delta_prev"] + dod_us
    ts_us    = state["ts_us"] + delta_us
    ts_s     = ts_us / TS_SCALE

    state["ts_delta_prev"] = delta_us
    state["ts_us"]         = ts_us

    record = {
        "timestamp": ts_s,
        "datetime":  _ts_to_str(ts_s),
        "accel_samples": None,
        "inclin": None,
        "temp":   None,
        "record_type": "DELTA",
    }

    if header & FLAG_ACCEL:
        (n,) = read_fmt(f, "<B", "accel sample count")
        cur = state["accel_prev"][:]
        samples_int = []
        for _ in range(n):
            dx, dy, dz = read_fmt(f, "<hhh", "accel delta sample")
            cur = [cur[0]+dx, cur[1]+dy, cur[2]+dz]
            samples_int.append(cur[:])
        if samples_int:
            state["accel_prev"] = samples_int[-1]
        record["accel_samples"] = [(x/ACCEL_SCALE, y/ACCEL_SCALE, z/ACCEL_SCALE)
                                   for x,y,z in samples_int] if samples_int else None

    if header & FLAG_INCLIN:
        dr, dp, dy_ = read_fmt(f, "<hhh", "inclin delta")
        prev = state["inclin_prev"]
        state["inclin_prev"] = [prev[0]+dr, prev[1]+dp, prev[2]+dy_]
        record["inclin"] = tuple(v/INCLIN_SCALE for v in state["inclin_prev"])

    if header & FLAG_TEMP:
        (dt,) = read_fmt(f, "<h", "temp delta")
        state["temp_prev"] = state["temp_prev"] + dt
        record["temp"] = state["temp_prev"] / TEMP_SCALE

    return record


# ──────────────────────────────────────────────────────────────────
# Output helpers
# ──────────────────────────────────────────────────────────────────

def _ts_to_str(ts_s: float) -> str:
    try:
        return datetime.fromtimestamp(ts_s, tz=timezone.utc).strftime("%Y-%m-%d %H:%M:%S.%f")
    except Exception:
        return f"<invalid ts={ts_s}>"


def _print_record(rec: dict):
    idx  = rec.get("record_index", "?")
    kind = rec["record_type"]
    dt   = rec["datetime"]
    ts   = rec["timestamp"]

    print(f"\n── Record #{idx} [{kind}] ──────────────────────")
    print(f"  Timestamp : {ts:.6f} s  ({dt})")

    if rec["accel_samples"] is not None:
        for i, (x, y, z) in enumerate(rec["accel_samples"]):
            print(f"  Accel[{i}]  : x={x:+.4f}  y={y:+.4f}  z={z:+.4f}  g")

    if rec["inclin"] is not None:
        r, p, y = rec["inclin"]
        print(f"  Inclin    : roll={r:+.4f}  pitch={p:+.4f}  yaw={y:+.4f}  °")

    if rec["temp"] is not None:
        print(f"  Temp      : {rec['temp']:+.2f} °C")


def print_summary(records: list, filepath: str):
    print(f"\n{'='*60}")
    print(f"  File    : {os.path.basename(filepath)}")
    print(f"  Records : {len(records)}")
    if records:
        ts_first = records[0]["timestamp"]
        ts_last  = records[-1]["timestamp"]
        span_s   = ts_last - ts_first
        abs_cnt  = sum(1 for r in records if r["record_type"] == "ABSOLUTE")
        dlt_cnt  = len(records) - abs_cnt
        print(f"  Time span   : {span_s:.3f} s  ({_ts_to_str(ts_first)}  →  {_ts_to_str(ts_last)})")
        print(f"  ABSOLUTE records : {abs_cnt}")
        print(f"  DELTA    records : {dlt_cnt}")

        has_accel  = [r for r in records if r["accel_samples"]]
        has_inclin = [r for r in records if r["inclin"] is not None]
        has_temp   = [r for r in records if r["temp"]  is not None]

        if has_accel:
            all_x = [s[0] for r in has_accel for s in r["accel_samples"]]
            all_y = [s[1] for r in has_accel for s in r["accel_samples"]]
            all_z = [s[2] for r in has_accel for s in r["accel_samples"]]
            print(f"  Accel X range   : [{min(all_x):+.4f}, {max(all_x):+.4f}] g")
            print(f"  Accel Y range   : [{min(all_y):+.4f}, {max(all_y):+.4f}] g")
            print(f"  Accel Z range   : [{min(all_z):+.4f}, {max(all_z):+.4f}] g")

        if has_inclin:
            rolls   = [r["inclin"][0] for r in has_inclin]
            pitches = [r["inclin"][1] for r in has_inclin]
            yaws    = [r["inclin"][2] for r in has_inclin]
            print(f"  Inclin roll     : [{min(rolls):+.4f}, {max(rolls):+.4f}] °")
            print(f"  Inclin pitch    : [{min(pitches):+.4f}, {max(pitches):+.4f}] °")
            print(f"  Inclin yaw      : [{min(yaws):+.4f}, {max(yaws):+.4f}] °")

        if has_temp:
            temps = [r["temp"] for r in has_temp]
            print(f"  Temp range      : [{min(temps):+.2f}, {max(temps):+.2f}] °C")

    print('='*60)


def export_csv(records: list, csv_path: str):
    """Flatten records to CSV (one row per accel sample; inclin/temp repeated)."""
    fieldnames = [
        "record_index", "record_type", "timestamp", "datetime",
        "accel_sample_idx", "accel_x", "accel_y", "accel_z",
        "inclin_roll", "inclin_pitch", "inclin_yaw",
        "temp_c",
    ]
    with open(csv_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for rec in records:
            base = {
                "record_index": rec["record_index"],
                "record_type":  rec["record_type"],
                "timestamp":    f"{rec['timestamp']:.6f}",
                "datetime":     rec["datetime"],
                "inclin_roll":  f"{rec['inclin'][0]:.4f}"  if rec["inclin"] else "",
                "inclin_pitch": f"{rec['inclin'][1]:.4f}"  if rec["inclin"] else "",
                "inclin_yaw":   f"{rec['inclin'][2]:.4f}"  if rec["inclin"] else "",
                "temp_c":       f"{rec['temp']:.2f}"       if rec["temp"] is not None else "",
            }
            if rec["accel_samples"]:
                for i, (x, y, z) in enumerate(rec["accel_samples"]):
                    row = dict(base)
                    row["accel_sample_idx"] = i
                    row["accel_x"] = f"{x:.4f}"
                    row["accel_y"] = f"{y:.4f}"
                    row["accel_z"] = f"{z:.4f}"
                    writer.writerow(row)
            else:
                row = dict(base)
                row["accel_sample_idx"] = ""
                row["accel_x"] = row["accel_y"] = row["accel_z"] = ""
                writer.writerow(row)

    print(f"CSV exported → {csv_path}  ({len(records)} records)")


# ──────────────────────────────────────────────────────────────────
# CLI entry point
# ──────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Decode binary sensor files written by multiple_nodes_test.py"
    )
    parser.add_argument("file", help="Path to .bin file to decode")
    parser.add_argument("--csv",     metavar="OUTPUT.csv",
                        help="Export decoded records to CSV")
    parser.add_argument("--verbose", action="store_true",
                        help="Print every record (can be long for large files)")
    parser.add_argument("--head",    type=int, metavar="N", default=0,
                        help="Print only the first N records (implies --verbose for those N)")
    args = parser.parse_args()

    if not os.path.isfile(args.file):
        print(f"[ERROR] File not found: {args.file}", file=sys.stderr)
        sys.exit(1)

    print(f"Decoding: {args.file}  ({os.path.getsize(args.file):,} bytes)")

    records = decode_file(args.file, verbose=False)   # collect first, print after

    # --head: print first N records verbosely
    if args.head > 0:
        for rec in records[:args.head]:
            _print_record(rec)
    elif args.verbose:
        for rec in records:
            _print_record(rec)

    print_summary(records, args.file)

    if args.csv:
        export_csv(records, args.csv)


if __name__ == "__main__":
    main()