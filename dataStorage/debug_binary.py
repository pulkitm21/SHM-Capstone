"""
debug_binary.py  — per-sensor-timestamp edition
------------------------------------------------
Three-pass diagnostic tool for binary files from delta_encoder.py.

  Pass 1 — Structural scan    (raw bytes, no reconstruction)
  Pass 2 — Delta trace        (full reconstruction, state printed per record)
  Pass 3 — Anomaly report     (root-cause hints)

Usage:
    python debug_binary.py <file.zst>
    python debug_binary.py <file.zst> --pass 1
    python debug_binary.py <file.zst> --pass 2
    python debug_binary.py <file.zst> --pass 3
    python debug_binary.py <file.zst> --record 5     # zoom into record #5 ±2
"""

import struct, sys, os, argparse, io
import zstandard as zstd
from datetime import datetime, timezone

TEMP_SCALE    = 100
ACCEL_SCALE   = 10000
INCLIN_SCALE  = 10000
TS_SCALE      = 1_000_000

FLAG_ACCEL  = 0x01
FLAG_INCLIN = 0x02
FLAG_TEMP   = 0x04
SENTINEL    = 0xFF


# Per-sensor inter-packet jump thresholds (seconds).
# Must be >= the encoder's MAX_DELTA_S (60 s) to avoid false positives
# on slow sensors (e.g. temperature sampled once per minute).
# Set tighter for accel/inclin since those are high-rate sensors.
MAX_TS_JUMP_ACCEL  = 10.0   # 10 Hz → expect ~0.1 s, warn if > 10 s
MAX_TS_JUMP_INCLIN = 10.0   # same rate as accel in current firmware
MAX_TS_JUMP_TEMP   = 120.0  # sampled ~once/min; warn only if > 2 min gap
DOD_WARN_THRESH = 2**30
TS_MIN          = 1_577_836_800.0
TS_MAX          = 4_102_444_800.0


# ── Zstd decompression helper ─────────────────────────────────────

def open_decompressed(filepath: str) -> io.BytesIO:
    """
    Return a BytesIO of the file contents ready for the record decoders.

    .zst  — decompressed from a standard single-frame Zstd file
            (written by compress_and_replace() at hour boundary).
    .bin  — returned as-is (current hour file, still being written).

    Uses stream_reader() instead of decompress() so that files compressed
    without a content-size header (the default for copy_stream) are handled
    correctly.
    """
    with open(filepath, "rb") as f:
        raw = f.read()
    if not filepath.endswith(".zst"):
        return io.BytesIO(raw)
    dctx = zstd.ZstdDecompressor()
    buf  = bytearray()
    with dctx.stream_reader(io.BytesIO(raw)) as reader:
        while True:
            chunk = reader.read(65536)
            if not chunk:
                break
            buf += chunk
    return io.BytesIO(bytes(buf))
    dctx = zstd.ZstdDecompressor()
    return io.BytesIO(dctx.decompress(raw))

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
    if not (TS_MIN <= ts_s <= TS_MAX):
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

    with open_decompressed(filepath) as f:
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
                                      f"x={x/ACCEL_SCALE:+.4f}  y={y/ACCEL_SCALE:+.4f}  z={z/ACCEL_SCALE:+.4f} g")

                    if header & FLAG_INCLIN:
                        (ts_us,) = read_fmt(f, "<q", "inclin ts")
                        r, p, y  = read_fmt(f, "<iii", "inclin xyz")
                        _check_ts(ts_us, idx, "ABSOLUTE/inclin", issues)
                        if show:
                            ts_s = ts_us / TS_SCALE
                            print(f"  │  inclin ts={ts_us}µs ({ts_str(ts_s)})  "
                                  f"r={r/INCLIN_SCALE:+.4f}  p={p/INCLIN_SCALE:+.4f}  y={y/INCLIN_SCALE:+.4f} °")

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
                            (changed,)   = read_fmt(f, "<B", f"accel[{i}] changed")
                            delta_ts = dx = dy = dz = None
                            if changed & 0x01:
                                (delta_ts,) = read_fmt(f, "<i", f"accel[{i}] delta_ts")
                                if abs(delta_ts) > DOD_WARN_THRESH:
                                    issues.append((idx, "DELTA/accel", f"delta_ts={delta_ts} near int32 overflow"))
                            if changed & 0x02: (dx,) = read_fmt(f, "<h", f"accel[{i}] dx")
                            if changed & 0x04: (dy,) = read_fmt(f, "<h", f"accel[{i}] dy")
                            if changed & 0x08: (dz,) = read_fmt(f, "<h", f"accel[{i}] dz")
                            if show:
                                dts_s = f"{delta_ts}µs" if delta_ts is not None else "null"
                                dx_s  = f"{dx/ACCEL_SCALE:+.4f}" if dx is not None else "null"
                                dy_s  = f"{dy/ACCEL_SCALE:+.4f}" if dy is not None else "null"
                                dz_s  = f"{dz/ACCEL_SCALE:+.4f}" if dz is not None else "null"
                                print(f"  │    [{i}] changed=0x{changed:02X}  delta_ts={dts_s}  "
                                      f"Δx={dx_s}  Δy={dy_s}  Δz={dz_s} g")

                    if header & FLAG_INCLIN:
                        (changed,) = read_fmt(f, "<B", "inclin changed")
                        delta_ts = dr = dp = dy = None
                        if changed & 0x01:
                            (delta_ts,) = read_fmt(f, "<i", "inclin delta_ts")
                            if abs(delta_ts) > DOD_WARN_THRESH:
                                issues.append((idx, "DELTA/inclin", f"delta_ts={delta_ts} near int32 overflow"))
                        if changed & 0x02: (dr,) = read_fmt(f, "<h", "inclin dr")
                        if changed & 0x04: (dp,) = read_fmt(f, "<h", "inclin dp")
                        if changed & 0x08: (dy,) = read_fmt(f, "<h", "inclin dyaw")
                        for v, name in [(dr,"dr"),(dp,"dp")]:
                            if v is not None and abs(v) == 32767:
                                issues.append((idx, "DELTA/inclin", f"{name} hit int16 max — likely clipped"))
                        if show:
                            dts_s = f"{delta_ts}µs" if delta_ts is not None else "null"
                            dr_s  = f"{dr/INCLIN_SCALE:+.4f}" if dr is not None else "null"
                            dp_s  = f"{dp/INCLIN_SCALE:+.4f}" if dp is not None else "null"
                            dy_s  = f"{dy/INCLIN_SCALE:+.4f}" if dy is not None else "null"
                            print(f"  │  inclin changed=0x{changed:02X}  delta_ts={dts_s}  "
                                  f"Δr={dr_s}  Δp={dp_s}  Δy={dy_s} °")

                    if header & FLAG_TEMP:
                        (changed,) = read_fmt(f, "<B", "temp changed")
                        delta_ts = dt = None
                        if changed & 0x01:
                            (delta_ts,) = read_fmt(f, "<i", "temp delta_ts")
                            if abs(delta_ts) > DOD_WARN_THRESH:
                                issues.append((idx, "DELTA/temp", f"delta_ts={delta_ts} near int32 overflow"))
                        if changed & 0x02:
                            (dt,) = read_fmt(f, "<h", "temp dval")
                            if abs(dt) == 32767:
                                issues.append((idx, "DELTA/temp", f"Δtemp={dt} hit int16 max — likely clipped"))
                        if show:
                            dts_s = f"{delta_ts}µs" if delta_ts is not None else "null"
                            dt_s  = f"{dt/TEMP_SCALE:+.4f}" if dt is not None else "null"
                            print(f"  │  temp  changed=0x{changed:02X}  delta_ts={dts_s}  Δval={dt_s} °C")

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
    return {"ts_us": 0, "xyz_prev": [0, 0, 0], "val_prev": 0, "first_ts_us": None}  # first sample ts of previous packet (for jump check)

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

    def fmt_a(v): return f"{v/ACCEL_SCALE:+.4f}"
    def fmt_i(v): return f"{v/INCLIN_SCALE:+.4f}"
    def fmt_t(v): return f"{v/TEMP_SCALE:.2f}"

    def _abs_ts(f, ss, label, is_first_sample=False):
        (ts_us,) = read_fmt(f, "<q", label)
        if is_first_sample:
            ss["first_ts_us"] = ts_us
        ss["ts_us"] = ts_us
        return ts_us

    def _delta_ts(ss, delta_us, label, show, rec_idx, is_first_sample=False):
        if show:
            print(f"    {label}: delta_ts={delta_us}µs  "
                  f"ts_us={ts_us}  ({ts_str(ts_us/TS_SCALE)})")
        # For the first sample of a burst, compare against the first sample of the
        # previous packet — not the last sample — to avoid false negatives caused
        # by back-spacing (e.g. accel[0].ts = now - (n-1)*dt is always behind
        # accel[n-1].ts of the previous packet by design, not a real anomaly).
        # Pick threshold based on which sensor this label belongs to
        if "temp" in label.lower():
            _jump_thresh = MAX_TS_JUMP_TEMP
        elif "inclin" in label.lower():
            _jump_thresh = MAX_TS_JUMP_INCLIN
        else:
            _jump_thresh = MAX_TS_JUMP_ACCEL
        if is_first_sample and ss["first_ts_us"] is not None:
            jump_s = (ts_us - ss["first_ts_us"]) / TS_SCALE
            # Ignore tiny negative steps (< 1 ms) — these are µs-level dod
            # noise around a frozen timestamp (e.g. temp not yet updated by ADC).
            if abs(jump_s) > _jump_thresh or jump_s < -0.001:
                msg = f"{label} jump {jump_s:+.3f} s (first-to-first)"
                issues.append((rec_idx, "DELTA", msg))
                if show: print(f"    ⚠ {msg}")
        elif not is_first_sample:
            # Within a burst: each sample should be a small positive step
            prev_ts = ss["ts_us"]
            jump_s  = (ts_us - prev_ts) / TS_SCALE
            if jump_s < 0 or jump_s > _jump_thresh:
                msg = f"{label} within-burst jump {jump_s:+.3f} s"
                issues.append((rec_idx, "DELTA", msg))
                if show: print(f"    ⚠ {msg}")
        if is_first_sample:
            ss["first_ts_us"] = ts_us
        ss["ts_us"] = ts_us
        return ts_us

    with open_decompressed(filepath) as f:
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
                            ts_us = _abs_ts(f, state["accel"], f"accel[{i}].ts",
                                            is_first_sample=(i == 0))
                            x, y, z = read_fmt(f, "<iii", f"accel[{i}]")
                            state["accel"]["xyz_prev"] = [x, y, z]
                            if show:
                                print(f"    accel[{i}] ts={ts_us}µs ({ts_str(ts_us/TS_SCALE)})  "
                                      f"x={fmt_a(x)}  y={fmt_a(y)}  z={fmt_a(z)} g")
                                print(f"    state.accel.xyz_prev ← {[x,y,z]}  ({[fmt_a(v) for v in [x,y,z]]})")

                    if header & FLAG_INCLIN:
                        ts_us = _abs_ts(f, state["inclin"], "inclin.ts", is_first_sample=True)
                        r, p, y = read_fmt(f, "<iii", "inclin")
                        state["inclin"]["xyz_prev"] = [r, p, y]
                        if show:
                            print(f"    inclin ts={ts_us}µs ({ts_str(ts_us/TS_SCALE)})  "
                                  f"r={fmt_i(r)}  p={fmt_i(p)}  y={fmt_i(y)} °")
                            print(f"    state.inclin.xyz_prev ← {[r,p,y]}")

                    if header & FLAG_TEMP:
                        ts_us = _abs_ts(f, state["temp"], "temp.ts", is_first_sample=True)
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
                        ss   = state["accel"]
                        prev = ss["xyz_prev"]
                        for i in range(n):
                            (changed,) = read_fmt(f, "<B", f"accel[{i}] changed")
                            if changed & 0x01:
                                (delta_us,) = read_fmt(f, "<i", f"accel[{i}].ts")
                                ts_us       = _delta_ts(ss, delta_us, f"accel[{i}].ts",
                                                        show, idx, is_first_sample=(i == 0))
                            else:
                                ts_us = ss["ts_us"]
                                if show: print(f"    accel[{i}].ts: null (unchanged)")
                            dx = read_fmt(f, "<h", f"accel[{i}] dx")[0] if changed & 0x02 else 0
                            dy = read_fmt(f, "<h", f"accel[{i}] dy")[0] if changed & 0x04 else 0
                            dz = read_fmt(f, "<h", f"accel[{i}] dz")[0] if changed & 0x08 else 0
                            cur = [prev[0]+dx, prev[1]+dy, prev[2]+dz]
                            if show:
                                dx_s = str(dx) if changed & 0x02 else "null"
                                dy_s = str(dy) if changed & 0x04 else "null"
                                dz_s = str(dz) if changed & 0x08 else "null"
                                print(f"    accel[{i}] changed=0x{changed:02X}  Δ=({dx_s},{dy_s},{dz_s})  "
                                      f"prev={[fmt_a(v) for v in prev]}  "
                                      f"→ now={[fmt_a(v) for v in cur]} g")
                            prev = cur
                        ss["xyz_prev"] = prev

                    if header & FLAG_INCLIN:
                        ss = state["inclin"]
                        (changed,) = read_fmt(f, "<B", "inclin changed")
                        if changed & 0x01:
                            (delta_us,) = read_fmt(f, "<i", "inclin.ts")
                            ts_us       = _delta_ts(ss, delta_us, "inclin.ts", show, idx, is_first_sample=True)
                        else:
                            ts_us = ss["ts_us"]
                            if show: print(f"    inclin.ts: null (unchanged)")
                        prev = ss["xyz_prev"]
                        dr  = read_fmt(f, "<h", "inclin dr")[0]   if changed & 0x02 else 0
                        dp  = read_fmt(f, "<h", "inclin dp")[0]   if changed & 0x04 else 0
                        dy_ = read_fmt(f, "<i", "inclin dyaw")[0] if changed & 0x08 else 0
                        cur = [prev[0]+dr, prev[1]+dp, prev[2]+dy_]
                        ss["xyz_prev"] = cur
                        if show:
                            dr_s  = str(dr)  if changed & 0x02 else "null"
                            dp_s  = str(dp)  if changed & 0x04 else "null"
                            dy_s  = str(dy_) if changed & 0x08 else "null"
                            print(f"    inclin changed=0x{changed:02X}  Δ=({dr_s},{dp_s},{dy_s})  "
                                  f"prev={[fmt_i(v) for v in prev]}  "
                                  f"→ now={[fmt_i(v) for v in cur]} °")

                    if header & FLAG_TEMP:
                        ss = state["temp"]
                        (changed,) = read_fmt(f, "<B", "temp changed")
                        if changed & 0x01:
                            (delta_us,) = read_fmt(f, "<i", "temp.ts")
                            ts_us       = _delta_ts(ss, delta_us, "temp.ts", show, idx, is_first_sample=True)
                        else:
                            ts_us = ss["ts_us"]
                            if show: print(f"    temp.ts: null (unchanged)")
                        prev = ss["val_prev"]
                        if changed & 0x02:
                            (dt,) = read_fmt(f, "<h", "temp Δ")
                            cur   = prev + dt
                            ss["val_prev"] = cur
                        else:
                            dt  = None
                            cur = prev
                        if show:
                            dt_s = f"{dt} ({dt/TEMP_SCALE:+.2f} °C)" if dt is not None else "null"
                            print(f"    temp changed=0x{changed:02X}  Δ={dt_s}  "
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
      This file predates the ISO 8601 timestamp format change.
      Current firmware sends "2026-03-21T21:26:57.519349Z" strings
      which delta_encoder.py parses via parse_iso_timestamp().
""")

    if any("out of plausible range" in m for _,_,m in all_issues):
        print("""
  ⚠ Implausible timestamp (clock not synced at boot):
      Fix: add After=time-sync.target to systemd service,
           or fit a DS3231 RTC module to the Pi.
""")

    if any("near int32 overflow" in m for _,_,m in all_issues):
        print("""
  ⚠ delta_ts near int32 overflow:
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
      Thresholds: accel/inclin > 10 s, temp > 120 s.
      Possible causes: NTP step, MQTT QoS-0 reorder, broker restart,
      sensor sampling rate change, or gap before the overflow fix.
      If temp is flagging: check MAX_TS_JUMP_TEMP matches actual
      temperature sampling interval.
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

    compressed_size = os.path.getsize(args.file)
    if args.file.endswith(".zst"):
        _buf = open_decompressed(args.file)
        uncompressed_size = len(_buf.getvalue())
        ratio = compressed_size / uncompressed_size * 100 if uncompressed_size else 0
        print(f"Debug target : {args.file}")
        print(f"  Compressed   : {compressed_size:>10,} bytes")
        print(f"  Uncompressed : {uncompressed_size:>10,} bytes  (ratio {ratio:.1f}%)")
    else:
        print(f"Debug target : {args.file}  ({compressed_size:,} bytes)")
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