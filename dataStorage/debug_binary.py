"""
debug_binary.py
---------------
Diagnostic tool for investigating encoder / decoder mismatches in
binary files written by multiple_nodes_test.py.

Runs three analysis passes:

  Pass 1 — Structural scan
      Walks the file byte-by-byte using the format rules and reports
      every record header, field flags, sample counts, and raw bytes.
      Does NOT apply delta reconstruction — shows raw stored values.

  Pass 2 — Delta reconstruction trace
      Re-decodes with full delta reconstruction and prints every field
      alongside the running state so you can see exactly where values
      diverge.

  Pass 3 — Consistency checks
      Flags specific anomalies:
        • Multiple ABSOLUTE records (encoder reset mid-file)
        • Timestamp going backwards or jumping > 10 s
        • Delta-of-delta overflow (value near ±2^31)
        • Accel/inclin/temp values outside plausible physical ranges
        • Mismatched record count vs file size estimate

Usage:
    python debug_binary.py <file.bin>
    python debug_binary.py <file.bin> --pass 1        # structural only
    python debug_binary.py <file.bin> --pass 2        # delta trace only
    python debug_binary.py <file.bin> --pass 3        # anomaly report only
    python debug_binary.py <file.bin> --record 5      # zoom into record #5 ±2
"""

import struct
import sys
import os
import argparse
from datetime import datetime, timezone

# ── Scale factors — must match encoder ────────────────────────────
TEMP_SCALE   = 100
ACCEL_SCALE  = 1000
INCLIN_SCALE = 1000
TS_SCALE     = 1_000_000   # µs → s

FLAG_ACCEL  = 0x01
FLAG_INCLIN = 0x02
FLAG_TEMP   = 0x04
SENTINEL    = 0xFF

# ── Physical plausibility limits ──────────────────────────────────
MAX_ACCEL_G     = 50.0      # g
MAX_INCLIN_DEG  = 360.0     # °
MAX_TEMP_C      = 200.0
MIN_TEMP_C      = -50.0
MAX_TS_JUMP_S   = 10.0      # seconds between records
DOD_WARN_THRESH = 2**30     # int32 near overflow

# Must match encoder
TS_MIN = 1_577_836_800.0   # 2020-01-01
TS_MAX = 4_102_444_800.0   # 2100-01-01


# ──────────────────────────────────────────────────────────────────
# Low-level helpers
# ──────────────────────────────────────────────────────────────────

class ParseError(Exception):
    pass

def read_fmt(f, fmt, label=""):
    n    = struct.calcsize(fmt)
    raw  = f.read(n)
    if len(raw) < n:
        raise EOFError(f"EOF reading {label!r}: got {len(raw)}/{n} bytes at offset {f.tell()}")
    return struct.unpack(fmt, raw), raw

def flag_str(header):
    parts = []
    if header & FLAG_ACCEL:  parts.append("ACCEL")
    if header & FLAG_INCLIN: parts.append("INCLIN")
    if header & FLAG_TEMP:   parts.append("TEMP")
    return "|".join(parts) if parts else "NONE"

def ts_str(ts_s):
    try:
        return datetime.fromtimestamp(ts_s, tz=timezone.utc).strftime("%H:%M:%S.%f")
    except Exception:
        return f"<invalid {ts_s:.3f}>"


# ──────────────────────────────────────────────────────────────────
# PASS 1 — Structural scan (raw values, no delta reconstruction)
# ──────────────────────────────────────────────────────────────────

def pass1_structural(filepath, focus_record=None):
    print(f"\n{'─'*70}")
    print(f"  PASS 1 — Structural scan: {os.path.basename(filepath)}")
    print(f"{'─'*70}")

    issues = []

    with open(filepath, "rb") as f:
        rec_idx = 0
        while True:
            offset = f.tell()
            b = f.read(1)
            if not b:
                break

            sentinel = b[0]
            show = focus_record is None or abs(rec_idx - focus_record) <= 2

            try:
                if sentinel == SENTINEL:
                    # ── ABSOLUTE record ───────────────────────────
                    (header,), _ = read_fmt(f, "<B",  "abs header")
                    (ts_us,),  _ = read_fmt(f, "<q",  "abs timestamp")
                    ts_s = ts_us / TS_SCALE

                    if show:
                        print(f"\n  ┌─ Record #{rec_idx}  [ABSOLUTE]  offset=0x{offset:06X}")
                        print(f"  │  header   = 0x{header:02X}  flags={flag_str(header)}")
                        print(f"  │  ts_us    = {ts_us}  →  {ts_s:.6f} s  ({ts_str(ts_s)})")

                    if not (TS_MIN <= ts_s <= TS_MAX):
                        try:
                            date_str = datetime.utcfromtimestamp(max(0, int(ts_s))).strftime("%Y-%m-%d")
                        except Exception:
                            date_str = "?"
                        issues.append((rec_idx, "ABSOLUTE",
                            f"Timestamp out of plausible range: {ts_s:.0f} s ({date_str}) — "
                            f"likely clock-not-synced on sensor node"))

                    if header & FLAG_ACCEL:
                        (n,), _ = read_fmt(f, "<B", "abs accel n")
                        if show: print(f"  │  accel n  = {n}")
                        for i in range(n):
                            (x, y, z), raw = read_fmt(f, "<iii", f"abs accel[{i}]")
                            xf, yf, zf = x/ACCEL_SCALE, y/ACCEL_SCALE, z/ACCEL_SCALE
                            if show:
                                print(f"  │    accel[{i}] raw=({x},{y},{z})  →  ({xf:+.4f},{yf:+.4f},{zf:+.4f}) g")
                            for v, name in [(abs(xf),"ax"),(abs(yf),"ay"),(abs(zf),"az")]:
                                if v > MAX_ACCEL_G:
                                    issues.append((rec_idx, "ABSOLUTE", f"{name} = {v:.3f} g exceeds {MAX_ACCEL_G} g"))

                    if header & FLAG_INCLIN:
                        (r, p, y), _ = read_fmt(f, "<iii", "abs inclin")
                        rf, pf, yf = r/INCLIN_SCALE, p/INCLIN_SCALE, y/INCLIN_SCALE
                        if show:
                            print(f"  │  inclin   raw=({r},{p},{y})  →  ({rf:+.4f},{pf:+.4f},{yf:+.4f}) °")

                    if header & FLAG_TEMP:
                        (t,), _ = read_fmt(f, "<i", "abs temp")
                        tf = t / TEMP_SCALE
                        if show:
                            print(f"  │  temp     raw={t}  →  {tf:.2f} °C")
                        if not (MIN_TEMP_C <= tf <= MAX_TEMP_C):
                            issues.append((rec_idx, "ABSOLUTE", f"Temp = {tf:.2f} °C out of range"))

                    if show: print(f"  └─")

                else:
                    # ── DELTA record ──────────────────────────────
                    header  = sentinel
                    (dod,), _ = read_fmt(f, "<i", "delta dod_ts")

                    if show:
                        print(f"\n  ┌─ Record #{rec_idx}  [DELTA]     offset=0x{offset:06X}")
                        print(f"  │  header   = 0x{header:02X}  flags={flag_str(header)}")
                        print(f"  │  dod_ts   = {dod} µs", end="")
                        if abs(dod) > DOD_WARN_THRESH:
                            print(f"  ← ⚠ NEAR int32 OVERFLOW", end="")
                            issues.append((rec_idx, "DELTA", f"dod_ts={dod} near int32 overflow"))
                        print()

                    if header & FLAG_ACCEL:
                        (n,), _ = read_fmt(f, "<B", "delta accel n")
                        if show: print(f"  │  accel n  = {n}")
                        for i in range(n):
                            (dx, dy, dz), _ = read_fmt(f, "<hhh", f"delta accel[{i}]")
                            if show:
                                print(f"  │    Δaccel[{i}] = ({dx},{dy},{dz})"
                                      f"  →  ({dx/ACCEL_SCALE:+.4f},{dy/ACCEL_SCALE:+.4f},{dz/ACCEL_SCALE:+.4f}) g-delta")

                    if header & FLAG_INCLIN:
                        (dr, dp, dy), _ = read_fmt(f, "<hhh", "delta inclin")
                        if show:
                            print(f"  │  Δinclin  = ({dr},{dp},{dy})"
                                  f"  →  ({dr/INCLIN_SCALE:+.4f},{dp/INCLIN_SCALE:+.4f},{dy/INCLIN_SCALE:+.4f}) °-delta")
                        for v, name in [(abs(dr),"dr"),(abs(dp),"dp"),(abs(dy),"dy")]:
                            if v == 32767:
                                issues.append((rec_idx, "DELTA", f"{name} hit int16 max (32767) — likely clipped"))

                    if header & FLAG_TEMP:
                        (dt,), _ = read_fmt(f, "<h", "delta temp")
                        if show:
                            print(f"  │  Δtemp    = {dt}  →  {dt/TEMP_SCALE:+.4f} °C-delta")
                        if abs(dt) == 32767:
                            issues.append((rec_idx, "DELTA", f"Δtemp={dt} hit int16 max — likely clipped"))

                    if show: print(f"  └─")

                rec_idx += 1

            except EOFError as e:
                print(f"\n  ⚠ Truncated at record #{rec_idx}: {e}")
                issues.append((rec_idx, "TRUNCATED", str(e)))
                break
            except struct.error as e:
                print(f"\n  ⚠ Struct error at record #{rec_idx}: {e}")
                issues.append((rec_idx, "STRUCT_ERROR", str(e)))
                break

    print(f"\n  Total records parsed: {rec_idx}")
    return issues


# ──────────────────────────────────────────────────────────────────
# PASS 2 — Delta reconstruction trace
# ──────────────────────────────────────────────────────────────────

def pass2_delta_trace(filepath, focus_record=None):
    print(f"\n{'─'*70}")
    print(f"  PASS 2 — Delta reconstruction trace")
    print(f"  (state kept as integer scaled units to avoid float accumulation)")
    print(f"{'─'*70}")

    # State stored as raw integer scaled units — NO floating point accumulation.
    # Convert to physical units only at print time.
    # accel: int * (1/ACCEL_SCALE) g
    # inclin: int * (1/INCLIN_SCALE) degrees
    # temp:   int * (1/TEMP_SCALE) °C
    # ts:     int µs
    state = {
        "ts_us":         0,       # int µs
        "ts_delta_prev": 0,       # int µs (previous first-delta, for dod)
        "accel_prev":    [0, 0, 0],   # int scaled units
        "inclin_prev":   [0, 0, 0],   # int scaled units
        "temp_prev":     0,            # int scaled units
    }

    def fmt_accel(v): return f"{v / ACCEL_SCALE:+.3f}"
    def fmt_inclin(v): return f"{v / INCLIN_SCALE:+.3f}"
    def fmt_temp(v):  return f"{v / TEMP_SCALE:.2f}"

    issues = []

    with open(filepath, "rb") as f:
        rec_idx = 0
        prev_ts_us = None

        while True:
            b = f.read(1)
            if not b:
                break

            sentinel = b[0]
            show = focus_record is None or abs(rec_idx - focus_record) <= 2

            try:
                if sentinel == SENTINEL:
                    (header,), _ = read_fmt(f, "<B", "header")
                    (ts_us,),  _ = read_fmt(f, "<q", "abs ts")
                    ts_s = ts_us / TS_SCALE

                    state["ts_us"]         = ts_us
                    state["ts_delta_prev"] = 0

                    if show:
                        print(f"\n  Record #{rec_idx} [ABSOLUTE]")
                        print(f"    ts = {ts_us} µs  →  {ts_s:.6f} s  ({ts_str(ts_s)})")
                        print(f"    state.ts_us         ← {ts_us}")
                        print(f"    state.ts_delta_prev ← 0")

                    if header & FLAG_ACCEL:
                        (n,), _ = read_fmt(f, "<B", "n")
                        samples_int = []
                        for _ in range(n):
                            (x,y,z), _ = read_fmt(f, "<iii", "abs accel")
                            samples_int.append([x, y, z])
                        if samples_int:
                            state["accel_prev"] = samples_int[-1]
                        if show:
                            disp = [f"{fmt_accel(v)} g" for v in state["accel_prev"]]
                            print(f"    state.accel_prev    ← {state['accel_prev']}  ({disp})")

                    if header & FLAG_INCLIN:
                        (r,p,y), _ = read_fmt(f, "<iii", "abs inclin")
                        state["inclin_prev"] = [r, p, y]
                        if show:
                            disp = [f"{fmt_inclin(v)} °" for v in state["inclin_prev"]]
                            print(f"    state.inclin_prev   ← {state['inclin_prev']}  ({disp})")

                    if header & FLAG_TEMP:
                        (t,), _ = read_fmt(f, "<i", "abs temp")
                        state["temp_prev"] = t
                        if show:
                            print(f"    state.temp_prev     ← {t}  ({fmt_temp(t)} °C)")

                else:
                    header = sentinel
                    (dod,), _ = read_fmt(f, "<i", "dod_ts")

                    delta_us    = state["ts_delta_prev"] + dod
                    ts_us_recon = state["ts_us"] + delta_us
                    ts_s        = ts_us_recon / TS_SCALE

                    if show:
                        print(f"\n  Record #{rec_idx} [DELTA]")
                        print(f"    dod_ts             = {dod} µs")
                        print(f"    delta_us           = ts_delta_prev({state['ts_delta_prev']}) + dod({dod}) = {delta_us}")
                        print(f"    ts_us_recon        = ts_us({state['ts_us']}) + delta({delta_us}) = {ts_us_recon}")
                        print(f"    ts_s               = {ts_s:.6f}  ({ts_str(ts_s)})")

                    if prev_ts_us is not None:
                        jump_s = (ts_us_recon - prev_ts_us) / TS_SCALE
                        if abs(jump_s) > MAX_TS_JUMP_S or jump_s < 0:
                            msg = f"Timestamp jump: {jump_s:+.3f} s (prev={prev_ts_us/TS_SCALE:.3f}, curr={ts_s:.3f})"
                            issues.append((rec_idx, "DELTA", msg))
                            if show:
                                print(f"    ⚠ {msg}")

                    state["ts_delta_prev"] = delta_us
                    state["ts_us"]         = ts_us_recon

                    if header & FLAG_ACCEL:
                        (n,), _ = read_fmt(f, "<B", "n")
                        prev = state["accel_prev"][:]
                        cur  = prev[:]
                        samples_int = []
                        for i in range(n):
                            (dx,dy,dz), _ = read_fmt(f, "<hhh", "accel delta")
                            cur = [cur[0]+dx, cur[1]+dy, cur[2]+dz]
                            samples_int.append(cur[:])
                        if samples_int:
                            state["accel_prev"] = samples_int[-1]
                        if show:
                            prev_disp = [fmt_accel(v) for v in prev]
                            now_disp  = [fmt_accel(v) for v in state["accel_prev"]]
                            print(f"    accel_prev was {prev} ({prev_disp})  →  now {state['accel_prev']} ({now_disp}) g")

                    if header & FLAG_INCLIN:
                        (dr,dp,dy_), _ = read_fmt(f, "<hhh", "inclin delta")
                        prev = state["inclin_prev"][:]
                        state["inclin_prev"] = [prev[0]+dr, prev[1]+dp, prev[2]+dy_]
                        if show:
                            prev_disp = [fmt_inclin(v) for v in prev]
                            now_disp  = [fmt_inclin(v) for v in state["inclin_prev"]]
                            print(f"    inclin_prev was {prev} ({prev_disp})  →  now {state['inclin_prev']} ({now_disp}) °")

                    if header & FLAG_TEMP:
                        (dt,), _ = read_fmt(f, "<h", "temp delta")
                        prev_t = state["temp_prev"]
                        state["temp_prev"] = prev_t + dt
                        if show:
                            print(f"    temp_prev was {prev_t} ({fmt_temp(prev_t)} °C)"
                                  f"  +  Δ{dt} ({dt/TEMP_SCALE:+.2f} °C)"
                                  f"  →  {state['temp_prev']} ({fmt_temp(state['temp_prev'])} °C)")

                prev_ts_us = ts_us if sentinel == SENTINEL else ts_us_recon
                rec_idx += 1

            except EOFError as e:
                print(f"\n  ⚠ Truncated at record #{rec_idx}: {e}")
                break

    return issues


# ──────────────────────────────────────────────────────────────────
# PASS 3 — Anomaly report
# ──────────────────────────────────────────────────────────────────

def pass3_anomalies(issues_p1, issues_p2):
    print(f"\n{'─'*70}")
    print(f"  PASS 3 — Anomaly report")
    print(f"{'─'*70}")

    all_issues = issues_p1 + issues_p2
    if not all_issues:
        print("\n  ✓ No anomalies detected.")
        return

    # Deduplicate and group by category
    seen = set()
    categories = {}
    for rec, kind, msg in all_issues:
        key = (rec, msg)
        if key in seen:
            continue
        seen.add(key)
        categories.setdefault(kind, []).append((rec, msg))

    for kind, items in sorted(categories.items()):
        print(f"\n  [{kind}]")
        for rec, msg in items:
            print(f"    Record #{rec:>4} : {msg}")

    print(f"\n  Total unique issues: {len(seen)}")

    # Root cause hints
    print(f"\n{'─'*70}")
    print("  LIKELY ROOT CAUSES")
    print(f"{'─'*70}")

    msgs = " ".join(m for _,_,m in all_issues)

    abs_count = len(categories.get("ABSOLUTE", []))
    if abs_count > 1:
        print(f"""
  \u2139  {abs_count} ABSOLUTE records in this file.
      With the fixed encoder this is expected whenever:
        \u2022 Hour boundary crossed (1 anchor per file).
        \u2022 Gap > MAX_DELTA_S (60 s) detected — encoder auto-emitted ABSOLUTE
          to prevent dod_ts int32 overflow.
        \u2022 Subscriber process was restarted mid-hour.
      If the count is unexpectedly high, check the encoder console for
      "Forcing ABSOLUTE record:" lines to see exact reasons.
""")

    if any("near int32 overflow" in m for _,_,m in all_issues):
        print("""
  \u26a0 dod_ts near int32 overflow detected in this file:
      This file was likely recorded with the OLD encoder before the fix.
      The fixed encoder (MAX_DELTA_S = 60 s) prevents this by emitting
      an ABSOLUTE record on any gap that would cause overflow.
      \u2192 Re-record data with the updated multiple_nodes_test.py.
""")

    if any("clipped" in m for _,_,m in all_issues):
        print("""
  \u26a0 int16 delta clipping detected in this file:
      This file was likely recorded with the OLD encoder before the fix.
      The fixed encoder's needs_absolute_record() detects impending clips
      and falls back to ABSOLUTE before they occur.
      \u2192 Re-record data with the updated multiple_nodes_test.py.
""")

    if any("out of plausible range" in m for _,_,m in all_issues):
        print("""
  ⚠ Implausible timestamp (clock not synced):
      The sensor node published packets before its system clock was
      set correctly (common on Pi boards with no RTC battery).
      The fixed encoder now drops these packets in on_message().
      To also fix it on the node side, either:
        1. Add a systemd service dependency: After=time-sync.target
        2. Have the node firmware wait until its own clock reads > TS_MIN
           before starting to publish.
        3. Fit a DS3231 RTC module so the Pi has a valid time at boot.
""")

    if any("Timestamp jump" in m for _,_,m in all_issues):
        print("""
  \u26a0 Timestamp jumped backwards or > 10 s:
      Possible causes even with the fixed encoder:
        1. NTP clock step on the Pi mid-session.
        2. MQTT QoS 0 out-of-order delivery.
        3. File recorded with old encoder before the overflow fix.
      Check encoder console for "Forcing ABSOLUTE" lines around the
      affected record index.
""")


# ──────────────────────────────────────────────────────────────────
# CLI
# ──────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Diagnose encoder/decoder mismatches in sensor binary files"
    )
    parser.add_argument("file",      help="Path to .bin file")
    parser.add_argument("--pass",    type=int, choices=[1,2,3], dest="only_pass",
                        metavar="N", help="Run only pass N (default: all)")
    parser.add_argument("--record",  type=int, default=None, metavar="N",
                        help="Zoom into record N ± 2 (suppresses all other records)")
    args = parser.parse_args()

    if not os.path.isfile(args.file):
        print(f"[ERROR] File not found: {args.file}", file=sys.stderr)
        sys.exit(1)

    print(f"Debug target : {args.file}  ({os.path.getsize(args.file):,} bytes)")
    if args.record is not None:
        print(f"Focusing on  : record #{args.record} ± 2")

    issues_p1 = []
    issues_p2 = []

    if args.only_pass in (None, 1):
        issues_p1 = pass1_structural(args.file, focus_record=args.record)

    if args.only_pass in (None, 2):
        issues_p2 = pass2_delta_trace(args.file, focus_record=args.record)

    if args.only_pass in (None, 3):
        pass3_anomalies(issues_p1, issues_p2)


if __name__ == "__main__":
    main()