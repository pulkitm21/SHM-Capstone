"""
decode_binary.py  — per-sensor-timestamp edition
-------------------------------------------------
Decodes binary files written by delta_encoder.py.

Binary format recap:
  ABSOLUTE:  0xFF | header(B)
      accel:  n(B) + n×[ts(q) x(i) y(i) z(i)]
      inclin: ts(q) r(i) p(i) yaw(i)
      temp:   ts(q) val(i)
  DELTA:  header(B)
      accel:  n(B) + n×[changed(B) delta_ts?(i) dx?(h) dy?(h) dz?(h)]
      inclin: changed(B) delta_ts?(i) dr?(h) dp?(h) dyaw?(h)
      temp:   changed(B) delta_ts?(i) dval?(h)
  changed byte: bit0=ts, bit1=x/r/val, bit2=y/p, bit3=z/yaw
  Fields with bit=0 are omitted; decoder keeps previous value.

Usage:
    python decode_binary.py <file.bin.gz>
    python decode_binary.py <file.bin.gz> --csv out.csv
    python decode_binary.py <file.bin.gz> --verbose
    python decode_binary.py <file.bin.gz> --head 10
"""

import struct, sys, os, argparse, csv, io
import gzip
from datetime import datetime, timezone

TEMP_SCALE    = 100
ACCEL_SCALE   = 10000
INCLIN_SCALE  = 10000
TS_SCALE      = 1_000_000

FLAG_ACCEL  = 0x01
FLAG_INCLIN = 0x02
FLAG_TEMP   = 0x04
SENTINEL    = 0xFF

FORMAT_V1 = 1   # legacy: raw dod_ts, no changed byte
FORMAT_V2 = 2   # changed byte + simple delta_ts
FORMAT_V3 = 3   # explicit NaN flags + inclination bursts
INT32_NAN_SENTINEL = -2147483648
CHANGED_NAN_X = 0x10
CHANGED_NAN_Y = 0x20
CHANGED_NAN_Z = 0x40
CHANGED_NAN_TEMP = 0x10




# ── Decompression helper ──────────────────────────────────────────

def open_decompressed(filepath: str) -> io.BytesIO:
    """
    Return a BytesIO of the file contents ready for the record decoders.

    .bin.gz — decompressed from a gzip file (written by compress_and_replace).
    .bin    — returned as-is (current hour file, still being written).
    """
    if filepath.endswith(".bin.gz"):
        with gzip.open(filepath, "rb") as f:
            return io.BytesIO(f.read())
    with open(filepath, "rb") as f:
        return io.BytesIO(f.read())

# ── Low-level helpers ─────────────────────────────────────────────

def read_bytes(f, n, label=""):
    d = f.read(n)
    if len(d) < n:
        raise EOFError(f"EOF reading {label!r}: got {len(d)}/{n} bytes")
    return d

def read_fmt(f, fmt, label=""):
    raw = read_bytes(f, struct.calcsize(fmt), label)
    return struct.unpack(fmt, raw)

def ts_to_str(ts_s):
    try:
        return datetime.fromtimestamp(ts_s, tz=timezone.utc).strftime("%Y-%m-%d %H:%M:%S.%f")
    except Exception:
        return f"<invalid {ts_s}>"

def _decode_abs_component(raw: int, scale: int):
    return None if raw == INT32_NAN_SENTINEL else raw / scale

def _fmt_opt(v, fmt: str) -> str:
    return "NaN" if v is None else format(v, fmt)


# ── v1 legacy delta decoders (no changed byte, raw dod_ts) ───────

def _v1_reconstruct_ts(f, ss: dict) -> int:
    """v1: read dod_ts int32, reconstruct ts with dod accumulation."""
    (dod,)   = read_fmt(f, "<i", "v1 dod_ts")
    delta_us = ss.get("ts_delta_prev", 0) + dod
    ts_us    = ss["ts_us"] + delta_us
    ss["ts_us"]         = ts_us
    ss["ts_delta_prev"] = delta_us
    return ts_us

def _decode_accel_delta_v1(f, state):
    (n,) = read_fmt(f, "<B", "accel n")
    out  = []
    ss   = state["accel"]
    prev = ss["xyz_prev"]
    for _ in range(n):
        ts_us    = _v1_reconstruct_ts(f, ss)
        dx,dy,dz = read_fmt(f, "<hhh", "accel delta xyz")
        cur = [prev[0]+dx, prev[1]+dy, prev[2]+dz]
        out.append((ts_us/TS_SCALE, cur[0]/ACCEL_SCALE, cur[1]/ACCEL_SCALE, cur[2]/ACCEL_SCALE))
        prev = cur
    ss["xyz_prev"] = prev
    return out or None

def _decode_inclin_delta_v1(f, state):
    ss       = state["inclin"]
    ts_us    = _v1_reconstruct_ts(f, ss)
    dr,dp,dy = read_fmt(f, "<hhh", "inclin delta")
    prev     = ss["xyz_prev"]
    cur      = [prev[0]+dr, prev[1]+dp, prev[2]+dy]
    ss["xyz_prev"] = cur
    return (ts_us/TS_SCALE, cur[0]/INCLIN_SCALE, cur[1]/INCLIN_SCALE, cur[2]/INCLIN_SCALE)

def _decode_temp_delta_v1(f, state):
    ss    = state["temp"]
    ts_us = _v1_reconstruct_ts(f, ss)
    (dt,) = read_fmt(f, "<h", "temp delta")
    val   = ss["val_prev"] + dt
    ss["val_prev"] = val
    return (ts_us/TS_SCALE, val/TEMP_SCALE)

# ── Per-sensor decoder state ─────────────────────────────────────
# Kept as integers throughout to prevent float accumulation.
#
# sensor_state = {
#   "ts_us":         int,   # last reconstructed timestamp (µs)
#   "ts_delta_prev": int,   # previous first-delta (for dod reconstruction)
#   "xyz_prev":      [int, int, int],   # accel / inclin
#   "val_prev":      int,               # temp only
# }

def _fresh_sensor_state():
    return {"ts_us": 0, "xyz_prev": [0, 0, 0], "val_prev": 0}

def _fresh_decode_state():
    return {
        "accel":  _fresh_sensor_state(),
        "inclin": _fresh_sensor_state(),
        "temp":   _fresh_sensor_state(),
    }


# ── Timestamp reconstruction ──────────────────────────────────────

def _reconstruct_abs_ts(f, ss: dict) -> int:
    """Read absolute int64 µs, update sensor state, return ts_us."""
    (ts_us,) = read_fmt(f, "<q", "abs timestamp")
    ss["ts_us"] = ts_us
    return ts_us


# ── Record decoders ───────────────────────────────────────────────

def _decode_accel_abs(f, state):
    """Returns list of (ts_s, x, y, z) tuples."""
    (n,) = read_fmt(f, "<B", "accel n")
    out  = []
    prev = list(state["accel"]["xyz_prev"])
    for _ in range(n):
        ts_us = _reconstruct_abs_ts(f, state["accel"])
        x_raw, y_raw, z_raw = read_fmt(f, "<iii", "accel abs xyz")
        vals = []
        for idx, raw in enumerate((x_raw, y_raw, z_raw)):
            if raw != INT32_NAN_SENTINEL:
                prev[idx] = raw
                vals.append(raw / ACCEL_SCALE)
            else:
                vals.append(None)
        out.append((ts_us / TS_SCALE, vals[0], vals[1], vals[2]))
    state["accel"]["xyz_prev"] = prev
    return out or None

def _decode_accel_delta(f, state, fv):
    (n,) = read_fmt(f, "<B", "accel n")
    out  = []
    ss   = state["accel"]
    prev = list(ss["xyz_prev"])
    for _ in range(n):
        (changed,) = read_fmt(f, "<B", "accel changed")
        nan_mask = (changed & 0x70) if fv >= FORMAT_V3 else 0
        low = changed & 0x0F
        if low & 0x01:
            (delta_us,) = read_fmt(f, "<i", "accel delta_ts")
            ts_us       = ss["ts_us"] + delta_us
            ss["ts_us"] = ts_us
        else:
            ts_us = ss["ts_us"]
        vals = []
        for idx, (delta_bit, nan_bit) in enumerate(((0x02, CHANGED_NAN_X), (0x04, CHANGED_NAN_Y), (0x08, CHANGED_NAN_Z))):
            if nan_mask & nan_bit:
                vals.append(None)
                continue
            d = read_fmt(f, "<h", "accel delta")[0] if (low & delta_bit) else 0
            prev[idx] += d
            vals.append(prev[idx] / ACCEL_SCALE)
        out.append((ts_us / TS_SCALE, vals[0], vals[1], vals[2]))
    ss["xyz_prev"] = prev
    return out or None

def _decode_inclin_abs(f, state, fv):
    if fv >= FORMAT_V3:
        (n,) = read_fmt(f, "<B", "inclin n")
        out = []
        prev = list(state["inclin"]["xyz_prev"])
        for _ in range(n):
            ts_us = _reconstruct_abs_ts(f, state["inclin"])
            r_raw, p_raw, y_raw = read_fmt(f, "<iii", "inclin abs xyz")
            vals = []
            for idx, raw in enumerate((r_raw, p_raw, y_raw)):
                if raw != INT32_NAN_SENTINEL:
                    prev[idx] = raw
                    vals.append(raw / INCLIN_SCALE)
                else:
                    vals.append(None)
            out.append((ts_us / TS_SCALE, vals[0], vals[1], vals[2]))
        state["inclin"]["xyz_prev"] = prev
        return out or None
    ts_us    = _reconstruct_abs_ts(f, state["inclin"])
    r, p, y  = read_fmt(f, "<iii", "inclin abs")
    state["inclin"]["xyz_prev"] = [r, p, y]
    return [(ts_us / TS_SCALE, r / INCLIN_SCALE, p / INCLIN_SCALE, y / INCLIN_SCALE)]

def _decode_inclin_delta(f, state, fv):
    ss = state["inclin"]
    prev = list(ss["xyz_prev"])
    if fv >= FORMAT_V3:
        (n,) = read_fmt(f, "<B", "inclin n")
        out = []
        for _ in range(n):
            (changed,) = read_fmt(f, "<B", "inclin changed")
            nan_mask = changed & 0x70
            low = changed & 0x0F
            if low & 0x01:
                (delta_us,) = read_fmt(f, "<i", "inclin delta_ts")
                ts_us       = ss["ts_us"] + delta_us
                ss["ts_us"] = ts_us
            else:
                ts_us = ss["ts_us"]
            vals = []
            for idx, (delta_bit, nan_bit) in enumerate(((0x02, CHANGED_NAN_X), (0x04, CHANGED_NAN_Y), (0x08, CHANGED_NAN_Z))):
                if nan_mask & nan_bit:
                    vals.append(None)
                    continue
                d = read_fmt(f, "<h", "inclin delta")[0] if (low & delta_bit) else 0
                prev[idx] += d
                vals.append(prev[idx] / INCLIN_SCALE)
            out.append((ts_us / TS_SCALE, vals[0], vals[1], vals[2]))
        ss["xyz_prev"] = prev
        return out or None
    (changed,) = read_fmt(f, "<B", "inclin changed")
    if changed & 0x01:
        (delta_us,) = read_fmt(f, "<i", "inclin delta_ts")
        ts_us       = ss["ts_us"] + delta_us
        ss["ts_us"] = ts_us
    else:
        ts_us = ss["ts_us"]
    dr = read_fmt(f, "<h", "inclin dr")[0] if changed & 0x02 else 0
    dp = read_fmt(f, "<h", "inclin dp")[0] if changed & 0x04 else 0
    dy = read_fmt(f, "<h", "inclin dy")[0] if changed & 0x08 else 0
    prev[0] += dr; prev[1] += dp; prev[2] += dy
    ss["xyz_prev"] = prev
    return [(ts_us / TS_SCALE, prev[0] / INCLIN_SCALE, prev[1] / INCLIN_SCALE, prev[2] / INCLIN_SCALE)]

def _decode_temp_abs(f, state):
    ts_us   = _reconstruct_abs_ts(f, state["temp"])
    (val,)  = read_fmt(f, "<i", "temp abs")
    if val != INT32_NAN_SENTINEL:
        state["temp"]["val_prev"] = val
        out = val / TEMP_SCALE
    else:
        out = None
    return (ts_us / TS_SCALE, out)

def _decode_temp_delta(f, state, fv):
    ss = state["temp"]
    (changed,) = read_fmt(f, "<B", "temp changed")
    low = changed & 0x0F
    if low & 0x01:
        (delta_us,) = read_fmt(f, "<i", "temp delta_ts")
        ts_us       = ss["ts_us"] + delta_us
        ss["ts_us"] = ts_us
    else:
        ts_us = ss["ts_us"]
    is_nan = (fv >= FORMAT_V3) and bool(changed & CHANGED_NAN_TEMP)
    if is_nan:
        return (ts_us / TS_SCALE, None)
    if low & 0x02:
        (dt,) = read_fmt(f, "<h", "temp dval")
        ss["val_prev"] = ss["val_prev"] + dt
    return (ts_us / TS_SCALE, ss["val_prev"] / TEMP_SCALE)


# ── File decoder ──────────────────────────────────────────────────

def decode_file(filepath: str, verbose: bool = False):
    records = []
    state   = _fresh_decode_state()

    with open_decompressed(filepath) as f:
        # First byte is the file format version
        ver = f.read(1)
        if not ver:
            return records
        fv = ver[0]
        if fv not in (FORMAT_V1, FORMAT_V2, FORMAT_V3):
            # No version byte — old file starting with sentinel 0xFF or header.
            # Seek back 1 byte and decode as v1 (original format).
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
                    rec = {"record_index": idx, "record_type": "ABSOLUTE",
                           "accel_samples": None, "inclin": None, "temp": None}
                    if header & FLAG_ACCEL:
                        rec["accel_samples"] = _decode_accel_abs(f, state)
                    if header & FLAG_INCLIN:
                        rec["inclin"] = _decode_inclin_abs(f, state, fv)
                    if header & FLAG_TEMP:
                        rec["temp"] = _decode_temp_abs(f, state)
                else:
                    header = sentinel
                    rec = {"record_index": idx, "record_type": "DELTA",
                           "accel_samples": None, "inclin": None, "temp": None}
                    if fv == FORMAT_V1:
                        if header & FLAG_ACCEL:
                            rec["accel_samples"] = _decode_accel_delta_v1(f, state)
                        if header & FLAG_INCLIN:
                            rec["inclin"] = _decode_inclin_delta_v1(f, state)
                        if header & FLAG_TEMP:
                            rec["temp"] = _decode_temp_delta_v1(f, state)
                    else:
                        if header & FLAG_ACCEL:
                            rec["accel_samples"] = _decode_accel_delta(f, state, fv)
                        if header & FLAG_INCLIN:
                            rec["inclin"] = _decode_inclin_delta(f, state, fv)
                        if header & FLAG_TEMP:
                            rec["temp"] = _decode_temp_delta(f, state, fv)

                records.append(rec)
                if verbose:
                    _print_record(rec)
                idx += 1

            except EOFError as e:
                print(f"\n[WARNING] Truncated at record #{idx}: {e}", file=sys.stderr)
                break
            except struct.error as e:
                print(f"\n[ERROR] Struct error at record #{idx}: {e}", file=sys.stderr)
                break

    return records


# ── Output helpers ────────────────────────────────────────────────

def _print_record(rec):
    print(f"\n── Record #{rec['record_index']} [{rec['record_type']}] ──")
    if rec["accel_samples"]:
        for i, (ts, x, y, z) in enumerate(rec["accel_samples"]):
            print(f"  Accel[{i}]  ts={ts:.6f} ({ts_to_str(ts)})  "
                  f"x={_fmt_opt(x, '+.4f')}  y={_fmt_opt(y, '+.4f')}  z={_fmt_opt(z, '+.4f')} g")
    if rec["inclin"]:
        for i, (ts, r, p, y) in enumerate(rec["inclin"]):
            print(f"  Inclin[{i}] ts={ts:.6f} ({ts_to_str(ts)})  "
                  f"roll={_fmt_opt(r, '+.4f')}  pitch={_fmt_opt(p, '+.4f')}  yaw={_fmt_opt(y, '+.4f')} °")
    if rec["temp"]:
        ts, v = rec["temp"]
        print(f"  Temp      ts={ts:.6f} ({ts_to_str(ts)})  {_fmt_opt(v, '.2f')} °C")

def print_summary(records, filepath):
    print(f"\n{'='*60}")
    print(f"  File    : {os.path.basename(filepath)}")
    print(f"  Records : {len(records)}")
    if not records:
        print('='*60); return

    abs_cnt = sum(1 for r in records if r["record_type"] == "ABSOLUTE")
    print(f"  ABSOLUTE: {abs_cnt}   DELTA: {len(records)-abs_cnt}")

    all_accel  = [s for r in records if r["accel_samples"] for s in r["accel_samples"]]
    all_inclin = [s for r in records if r["inclin"] for s in r["inclin"]]
    all_temp   = [r["temp"]   for r in records if r["temp"]]

    if all_accel:
        xs = [s[1] for s in all_accel if s[1] is not None]; ys = [s[2] for s in all_accel if s[2] is not None]; zs = [s[3] for s in all_accel if s[3] is not None]
        ts = [s[0] for s in all_accel]
        print(f"  Accel samples : {len(all_accel)}")
        print(f"    time span   : {ts_to_str(min(ts))}  →  {ts_to_str(max(ts))}")
        if xs: print(f"    X [{min(xs):+.4f}, {max(xs):+.4f}] g")
        if ys: print(f"    Y [{min(ys):+.4f}, {max(ys):+.4f}] g")
        if zs: print(f"    Z [{min(zs):+.4f}, {max(zs):+.4f}] g")

    if all_inclin:
        rs=[v[1] for v in all_inclin if v[1] is not None]; ps=[v[2] for v in all_inclin if v[2] is not None]; ys=[v[3] for v in all_inclin if v[3] is not None]
        ts=[v[0] for v in all_inclin]
        print(f"  Inclin samples: {len(all_inclin)}")
        print(f"    time span   : {ts_to_str(min(ts))}  →  {ts_to_str(max(ts))}")
        if rs: print(f"    roll  [{min(rs):+.4f}, {max(rs):+.4f}] °")
        if ps: print(f"    pitch [{min(ps):+.4f}, {max(ps):+.4f}] °")
        if ys: print(f"    yaw   [{min(ys):+.4f}, {max(ys):+.4f}] °")

    if all_temp:
        vs=[v[1] for v in all_temp if v[1] is not None]; ts=[v[0] for v in all_temp]
        print(f"  Temp samples  : {len(all_temp)}")
        print(f"    time span   : {ts_to_str(min(ts))}  →  {ts_to_str(max(ts))}")
        if vs: print(f"    [{min(vs):.2f}, {max(vs):.2f}] °C")
    print('='*60)

def export_csv(records, csv_path):
    fields = ["record_index","record_type","sample_kind","sample_idx","ts","x","y","z","value"]
    with open(csv_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for rec in records:
            if rec["accel_samples"]:
                for i, (ts, x, y, z) in enumerate(rec["accel_samples"]):
                    w.writerow({"record_index": rec["record_index"], "record_type": rec["record_type"], "sample_kind": "accel", "sample_idx": i, "ts": f"{ts:.6f}", "x": "" if x is None else f"{x:.4f}", "y": "" if y is None else f"{y:.4f}", "z": "" if z is None else f"{z:.4f}", "value": ""})
            if rec["inclin"]:
                for i, (ts, r, p, y) in enumerate(rec["inclin"]):
                    w.writerow({"record_index": rec["record_index"], "record_type": rec["record_type"], "sample_kind": "inclin", "sample_idx": i, "ts": f"{ts:.6f}", "x": "" if r is None else f"{r:.4f}", "y": "" if p is None else f"{p:.4f}", "z": "" if y is None else f"{y:.4f}", "value": ""})
            if rec["temp"]:
                ts, v = rec["temp"]
                w.writerow({"record_index": rec["record_index"], "record_type": rec["record_type"], "sample_kind": "temp", "sample_idx": 0, "ts": f"{ts:.6f}", "x": "", "y": "", "z": "", "value": "" if v is None else f"{v:.2f}"})
    print(f"CSV exported → {csv_path}  ({len(records)} records)")


# ── CLI ───────────────────────────────────────────────────────────

def main():
    p = argparse.ArgumentParser(description="Decode sensor binary files")
    p.add_argument("file")
    p.add_argument("--csv",     metavar="OUT.csv")
    p.add_argument("--verbose", action="store_true")
    p.add_argument("--head",    type=int, default=0, metavar="N")
    args = p.parse_args()

    if not os.path.isfile(args.file):
        print(f"[ERROR] Not found: {args.file}", file=sys.stderr); sys.exit(1)

    compressed_size = os.path.getsize(args.file)
    if args.file.endswith(".bin.gz"):
        # Decompress once to get uncompressed size for display
        import io as _io
        _buf = open_decompressed(args.file)
        uncompressed_size = len(_buf.getvalue())
        ratio = compressed_size / uncompressed_size * 100 if uncompressed_size else 0
        print(f"Decoding: {args.file}")
        print(f"  Compressed   : {compressed_size:>10,} bytes")
        print(f"  Uncompressed : {uncompressed_size:>10,} bytes  (ratio {ratio:.1f}%)")
    else:
        print(f"Decoding: {args.file}  ({compressed_size:,} bytes)")
    records = decode_file(args.file)

    if args.head > 0:
        for r in records[:args.head]: _print_record(r)
    elif args.verbose:
        for r in records: _print_record(r)

    print_summary(records, args.file)
    if args.csv:
        export_csv(records, args.csv)

if __name__ == "__main__":
    main()