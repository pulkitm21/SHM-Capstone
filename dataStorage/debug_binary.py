"""
debug_binary.py  — per-sensor-timestamp edition
------------------------------------------------
Three-pass diagnostic tool for binary files from delta_encoder.py.

  Pass 1 — Structural scan    (raw bytes, no reconstruction)
  Pass 2 — Delta trace        (full reconstruction, state printed per record)
  Pass 3 — Anomaly report     (root-cause hints)

Usage:
    python debug_binary.py <file.bin>
    python debug_binary.py <file.bin> --pass 1
    python debug_binary.py <file.bin> --pass 2
    python debug_binary.py <file.bin> --pass 3
    python debug_binary.py <file.bin> --record 5     # zoom into record #5 ±2
"""

import struct, sys, os, argparse
from datetime import datetime, timezone

TEMP_SCALE    = 100
ACCEL_SCALE   = 1000
INCLIN_SCALE  = 1000
TS_SCALE      = 1_000_000

FLAG_ACCEL  = 0x01
FLAG_INCLIN = 0x02
FLAG_TEMP   = 0x04
SENTINEL    = 0xFF

MAX_TS_JUMP_S   = 10.0
DOD_WARN_THRESH = 2**30
TS_MIN          = 1_577_836_800.0
TS_MAX          = 4_102_444_800.0
TS_MS_THRESHOLD = 1e11


def read_bytes(f, n, label=""):
    d = f.read(n)
    if len(d) < n:
        raise EOFError(f"EOF reading {label!r}: got {len(d)}/{n} bytes")
    return d

def read_fmt(f, fmt, label=""):
    raw = read_bytes(f, struct.calcsize(fmt), label)
    return struct.unpack(fmt, raw)

def flag_str(h):
    p = []
    if h & FLAG_ACCEL:  p.append("ACCEL")
    if h & FLAG_INCLIN: p.append("INCLIN")
    if h & FLAG_TEMP:   p.append("TEMP")
    return "|".join(p) or "NONE"

def ts_str(ts_s):
    try:
        return datetime.fromtimestamp(ts_s, tz=timezone.utc).strftime("%H:%M:%S.%f")
    except Exception:
        return f"<invalid {ts_s:.1f}>"

def _date_str(ts_s):
    try:
        return datetime.utcfromtimestamp(max(0, int(ts_s))).strftime("%Y-%m-%d")
    except Exception:
        return "?"

def _check_ts(ts_us, rec_idx, label, issues):
    ts_s = ts_us / TS_SCALE
    if ts_s > TS_MS_THRESHOLD:
        issues.append((rec_idx, label,
            f"Timestamp is milliseconds not seconds: {ts_us} µs → "
            f"{ts_s/1000:.3f} s ({_date_str(ts_s/1000)})"))
    elif not (TS_MIN <= ts_s <= TS_MAX):
        issues.append((rec_idx, label,
            f"Timestamp out of plausible range: {ts_s:.0f} s ({_date_str(ts_s)}) "
            f"— likely clock-not-synced"))


# ══════════════════════════════════════════════════════════════════
# PASS 1 — Structural scan
# ══════════════════════════════════════════════════════════════════

def pass1_structural(filepath, focus_record=None):
    print(f"\n{'─'*70}")
    print(f"  PASS 1 — Structural scan (raw values, no reconstruction)")
    print(f"{'─'*70}")
    issues = []

    with open(filepath, "rb") as f:
        idx = 0
        while True:
            offset = f.tell()
            b = f.read(1)
            if not b: break
            sentinel = b[0]
            show = focus_record is None or abs(idx - focus_record) <= 2

            try:
                if sentinel == SENTINEL:
                    (header,) = read_fmt(f, "<B", "abs header")
                    if show:
                        print(f"\n  ┌─ Record #{idx}  [ABSOLUTE]  offset=0x{offset:06X}")
                        print(f"  │  header = 0x{header:02X}  flags={flag_str(header)}")

                    if header & FLAG_ACCEL:
                        (n,) = read_fmt(f, "<B", "accel n")
                        if show: print(f"  │  accel n={n}")
                        for i in range(n):
                            (ts_us,) = read_fmt(f, "<q", f"accel[{i}] ts")
                            x, y, z  = read_fmt(f, "<iii", f"accel[{i}] xyz")
                            _check_ts(ts_us, idx, "ABSOLUTE/accel", issues)
                            if show:
                                ts_s = ts_us / TS_SCALE
                                print(f"  │    [{i}] ts={ts_us}µs ({ts_str(ts_s)})  "
                                      f"x={x/ACCEL_SCALE:+.3f}  y={y/ACCEL_SCALE:+.3f}  z={z/ACCEL_SCALE:+.3f} g")

                    if header & FLAG_INCLIN:
                        (ts_us,) = read_fmt(f, "<q", "inclin ts")
                        r, p, y  = read_fmt(f, "<iii", "inclin xyz")
                        _check_ts(ts_us, idx, "ABSOLUTE/inclin", issues)
                        if show:
                            ts_s = ts_us / TS_SCALE
                            print(f"  │  inclin ts={ts_us}µs ({ts_str(ts_s)})  "
                                  f"r={r/INCLIN_SCALE:+.3f}  p={p/INCLIN_SCALE:+.3f}  y={y/INCLIN_SCALE:+.3f} °")

                    if header & FLAG_TEMP:
                        (ts_us,) = read_fmt(f, "<q", "temp ts")
                        (val,)   = read_fmt(f, "<i", "temp val")
                        _check_ts(ts_us, idx, "ABSOLUTE/temp", issues)
                        if show:
                            ts_s = ts_us / TS_SCALE
                            print(f"  │  temp ts={ts_us}µs ({ts_str(ts_s)})  val={val/TEMP_SCALE:.2f} °C")

                    if show: print(f"  └─")

                else:
                    header = sentinel
                    if show:
                        print(f"\n  ┌─ Record #{idx}  [DELTA]  offset=0x{offset:06X}")
                        print(f"  │  header = 0x{header:02X}  flags={flag_str(header)}")

                    if header & FLAG_ACCEL:
                        (n,) = read_fmt(f, "<B", "accel n")
                        if show: print(f"  │  accel n={n}")
                        for i in range(n):
                            (dod,)       = read_fmt(f, "<i", f"accel[{i}] dod_ts")
                            dx, dy, dz   = read_fmt(f, "<hhh", f"accel[{i}] deltas")
                            if abs(dod) > DOD_WARN_THRESH:
                                issues.append((idx, "DELTA/accel", f"dod_ts={dod} near int32 overflow"))
                            if show:
                                print(f"  │    [{i}] dod_ts={dod}µs  "
                                      f"Δx={dx/ACCEL_SCALE:+.3f}  Δy={dy/ACCEL_SCALE:+.3f}  Δz={dz/ACCEL_SCALE:+.3f} g")
                                if abs(dod) > DOD_WARN_THRESH:
                                    print(f"  │        ⚠ dod_ts near int32 overflow")

                    if header & FLAG_INCLIN:
                        (dod,)     = read_fmt(f, "<i", "inclin dod_ts")
                        dr, dp, dy = read_fmt(f, "<hhh", "inclin deltas")
                        if abs(dod) > DOD_WARN_THRESH:
                            issues.append((idx, "DELTA/inclin", f"dod_ts={dod} near int32 overflow"))
                        for v, name in [(abs(dr),"dr"),(abs(dp),"dp"),(abs(dy),"dy")]:
                            if v == 32767:
                                issues.append((idx, "DELTA/inclin", f"{name} hit int16 max — likely clipped"))
                        if show:
                            print(f"  │  inclin dod_ts={dod}µs  "
                                  f"Δr={dr/INCLIN_SCALE:+.3f}  Δp={dp/INCLIN_SCALE:+.3f}  Δy={dy/INCLIN_SCALE:+.3f} °")

                    if header & FLAG_TEMP:
                        (dod,) = read_fmt(f, "<i", "temp dod_ts")
                        (dt,)  = read_fmt(f, "<h", "temp delta")
                        if abs(dod) > DOD_WARN_THRESH:
                            issues.append((idx, "DELTA/temp", f"dod_ts={dod} near int32 overflow"))
                        if abs(dt) == 32767:
                            issues.append((idx, "DELTA/temp", f"Δtemp={dt} hit int16 max — likely clipped"))
                        if show:
                            print(f"  │  temp  dod_ts={dod}µs  Δval={dt/TEMP_SCALE:+.4f} °C")

                    if show: print(f"  └─")

                idx += 1

            except EOFError as e:
                print(f"\n  ⚠ Truncated at record #{idx}: {e}")
                issues.append((idx, "TRUNCATED", str(e)))
                break
            except struct.error as e:
                print(f"\n  ⚠ Struct error at record #{idx}: {e}")
                break

    print(f"\n  Total records parsed: {idx}")
    return issues


# ══════════════════════════════════════════════════════════════════
# PASS 2 — Delta reconstruction trace
# ══════════════════════════════════════════════════════════════════

def _fresh_sensor_state():
    return {"ts_us": 0, "ts_delta_prev": 0, "xyz_prev": [0, 0, 0], "val_prev": 0}

def pass2_delta_trace(filepath, focus_record=None):
    print(f"\n{'─'*70}")
    print(f"  PASS 2 — Delta reconstruction trace")
    print(f"  (integer state accumulation — no float drift)")
    print(f"{'─'*70}")

    state = {
        "accel":  _fresh_sensor_state(),
        "inclin": _fresh_sensor_state(),
        "temp":   _fresh_sensor_state(),
    }
    issues = []

    def fmt_a(v): return f"{v/ACCEL_SCALE:+.3f}"
    def fmt_i(v): return f"{v/INCLIN_SCALE:+.3f}"
    def fmt_t(v): return f"{v/TEMP_SCALE:.2f}"

    def _abs_ts(f, ss, label):
        (ts_us,) = read_fmt(f, "<q", label)
        ss["ts_us"] = ts_us; ss["ts_delta_prev"] = 0
        return ts_us

    def _dod_ts(f, ss, label, show, rec_idx):
        (dod,)   = read_fmt(f, "<i", label)
        delta_us = ss["ts_delta_prev"] + dod
        ts_us    = ss["ts_us"] + delta_us
        if show:
            print(f"    {label}: dod={dod}µs  delta={delta_us}µs  "
                  f"ts_us={ts_us}  ({ts_str(ts_us/TS_SCALE)})")
        prev_ts = ss["ts_us"]
        jump_s  = (ts_us - prev_ts) / TS_SCALE
        if abs(jump_s) > MAX_TS_JUMP_S or jump_s < 0:
            msg = f"{label} jump {jump_s:+.3f} s"
            issues.append((rec_idx, "DELTA", msg))
            if show: print(f"    ⚠ {msg}")
        ss["ts_us"] = ts_us; ss["ts_delta_prev"] = delta_us
        return ts_us

    with open(filepath, "rb") as f:
        idx = 0
        while True:
            b = f.read(1)
            if not b: break
            sentinel = b[0]
            show = focus_record is None or abs(idx - focus_record) <= 2

            try:
                if sentinel == SENTINEL:
                    (header,) = read_fmt(f, "<B", "abs header")
                    if show:
                        print(f"\n  Record #{idx} [ABSOLUTE]")

                    if header & FLAG_ACCEL:
                        (n,) = read_fmt(f, "<B", "accel n")
                        for i in range(n):
                            ts_us = _abs_ts(f, state["accel"], f"accel[{i}].ts")
                            x, y, z = read_fmt(f, "<iii", f"accel[{i}]")
                            state["accel"]["xyz_prev"] = [x, y, z]
                            if show:
                                print(f"    accel[{i}] ts={ts_us}µs ({ts_str(ts_us/TS_SCALE)})  "
                                      f"x={fmt_a(x)}  y={fmt_a(y)}  z={fmt_a(z)} g")
                                print(f"    state.accel.xyz_prev ← {[x,y,z]}  ({[fmt_a(v) for v in [x,y,z]]})")

                    if header & FLAG_INCLIN:
                        ts_us = _abs_ts(f, state["inclin"], "inclin.ts")
                        r, p, y = read_fmt(f, "<iii", "inclin")
                        state["inclin"]["xyz_prev"] = [r, p, y]
                        if show:
                            print(f"    inclin ts={ts_us}µs ({ts_str(ts_us/TS_SCALE)})  "
                                  f"r={fmt_i(r)}  p={fmt_i(p)}  y={fmt_i(y)} °")
                            print(f"    state.inclin.xyz_prev ← {[r,p,y]}")

                    if header & FLAG_TEMP:
                        ts_us = _abs_ts(f, state["temp"], "temp.ts")
                        (val,) = read_fmt(f, "<i", "temp")
                        state["temp"]["val_prev"] = val
                        if show:
                            print(f"    temp ts={ts_us}µs ({ts_str(ts_us/TS_SCALE)})  "
                                  f"val={fmt_t(val)} °C")
                            print(f"    state.temp.val_prev ← {val}")

                else:
                    header = sentinel
                    if show:
                        print(f"\n  Record #{idx} [DELTA]")

                    if header & FLAG_ACCEL:
                        (n,) = read_fmt(f, "<B", "accel n")
                        prev = state["accel"]["xyz_prev"]
                        for i in range(n):
                            ts_us = _dod_ts(f, state["accel"], f"accel[{i}].ts", show, idx)
                            dx, dy, dz = read_fmt(f, "<hhh", f"accel[{i}] Δxyz")
                            cur = [prev[0]+dx, prev[1]+dy, prev[2]+dz]
                            if show:
                                print(f"    accel[{i}] Δ=({dx},{dy},{dz})  "
                                      f"prev={[fmt_a(v) for v in prev]}  "
                                      f"→ now={[fmt_a(v) for v in cur]} g")
                            prev = cur
                        state["accel"]["xyz_prev"] = prev

                    if header & FLAG_INCLIN:
                        ts_us = _dod_ts(f, state["inclin"], "inclin.ts", show, idx)
                        dr, dp, dy = read_fmt(f, "<hhh", "inclin Δ")
                        prev = state["inclin"]["xyz_prev"]
                        cur  = [prev[0]+dr, prev[1]+dp, prev[2]+dy]
                        state["inclin"]["xyz_prev"] = cur
                        if show:
                            print(f"    inclin Δ=({dr},{dp},{dy})  "
                                  f"prev={[fmt_i(v) for v in prev]}  "
                                  f"→ now={[fmt_i(v) for v in cur]} °")

                    if header & FLAG_TEMP:
                        ts_us = _dod_ts(f, state["temp"], "temp.ts", show, idx)
                        (dt,) = read_fmt(f, "<h", "temp Δ")
                        prev  = state["temp"]["val_prev"]
                        cur   = prev + dt
                        state["temp"]["val_prev"] = cur
                        if show:
                            print(f"    temp Δ={dt} ({dt/TEMP_SCALE:+.2f} °C)  "
                                  f"prev={fmt_t(prev)}  → now={fmt_t(cur)} °C")

                idx += 1

            except EOFError as e:
                print(f"\n  ⚠ Truncated at record #{idx}: {e}")
                break

    return issues


# ══════════════════════════════════════════════════════════════════
# PASS 3 — Anomaly report
# ══════════════════════════════════════════════════════════════════

def pass3_anomalies(issues_p1, issues_p2):
    print(f"\n{'─'*70}")
    print(f"  PASS 3 — Anomaly report")
    print(f"{'─'*70}")

    all_issues = issues_p1 + issues_p2
    if not all_issues:
        print("\n  ✓ No anomalies detected.")
        return

    seen       = set()
    categories = {}
    for rec, kind, msg in all_issues:
        key = (rec, msg)
        if key in seen: continue
        seen.add(key)
        categories.setdefault(kind, []).append((rec, msg))

    for kind, items in sorted(categories.items()):
        print(f"\n  [{kind}]")
        for rec, msg in items:
            print(f"    Record #{rec:>4} : {msg}")

    print(f"\n  Total unique issues: {len(seen)}")

    print(f"\n{'─'*70}")
    print("  ROOT CAUSE HINTS")
    print(f"{'─'*70}")

    msgs = " ".join(m for _,_,m in all_issues)

    abs_count = len(categories.get("ABSOLUTE", [])) + len(categories.get("ABSOLUTE/accel", [])) \
              + len(categories.get("ABSOLUTE/inclin", [])) + len(categories.get("ABSOLUTE/temp", []))
    if abs_count > 1:
        print(f"""
  ℹ  Multiple ABSOLUTE records ({abs_count} issues flagged).
      Normal causes: hour boundary, gap > MAX_DELTA_S (60 s), process restart.
      Check encoder console for "Forcing ABSOLUTE record:" lines.
""")

    if any("milliseconds not seconds" in m for _,_,m in all_issues):
        print("""
  ⚠ Sensor sending milliseconds instead of seconds:
      e.g. 1704084600000 ms  →  correct is 1704084600 s.
      delta_encoder.py auto-converts via normalise_timestamp().
      Long-term: fix sensor firmware to send Unix seconds.
""")

    if any("out of plausible range" in m for _,_,m in all_issues):
        print("""
  ⚠ Implausible timestamp (clock not synced at boot):
      Fix: add After=time-sync.target to systemd service,
           or fit a DS3231 RTC module to the Pi.
""")

    if any("near int32 overflow" in m for _,_,m in all_issues):
        print("""
  ⚠ dod_ts near int32 overflow:
      File recorded before the MAX_DELTA_S gap-detection fix.
      Re-record with updated delta_encoder.py.
""")

    if any("clipped" in m for _,_,m in all_issues):
        print("""
  ⚠ int16 delta clipping:
      A sensor value jumped more than int16 can hold in one step.
      The fixed encoder detects this and emits ABSOLUTE instead.
      Re-record with updated delta_encoder.py.
""")

    if any("jump" in m for _,_,m in all_issues):
        print("""
  ⚠ Timestamp jump detected (per-sensor):
      Possible causes: NTP step, MQTT QoS-0 reorder, broker restart,
      or gap before the overflow fix was applied.
""")


# ══════════════════════════════════════════════════════════════════
# CLI
# ══════════════════════════════════════════════════════════════════

def main():
    p = argparse.ArgumentParser(description="Debug sensor binary files")
    p.add_argument("file")
    p.add_argument("--pass",   type=int, choices=[1,2,3], dest="only_pass", metavar="N")
    p.add_argument("--record", type=int, default=None, metavar="N",
                   help="Zoom into record N ± 2")
    args = p.parse_args()

    if not os.path.isfile(args.file):
        print(f"[ERROR] Not found: {args.file}", file=sys.stderr); sys.exit(1)

    print(f"Debug target : {args.file}  ({os.path.getsize(args.file):,} bytes)")
    if args.record is not None:
        print(f"Focusing on  : record #{args.record} ± 2")

    issues_p1 = issues_p2 = []

    if args.only_pass in (None, 1):
        issues_p1 = pass1_structural(args.file, focus_record=args.record)

    if args.only_pass in (None, 2):
        issues_p2 = pass2_delta_trace(args.file, focus_record=args.record)

    if args.only_pass in (None, 3):
        pass3_anomalies(issues_p1, issues_p2)


if __name__ == "__main__":
    main()