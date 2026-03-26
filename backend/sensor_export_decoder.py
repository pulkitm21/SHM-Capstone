from __future__ import annotations

import gzip
import io
import struct
from contextlib import ExitStack, contextmanager
from typing import Dict, Generator, Any

from decode_binary import (
    read_fmt,
    _fresh_decode_state,
    _decode_accel_abs,
    _decode_accel_delta,
    _decode_accel_delta_v1,
    _decode_inclin_abs,
    _decode_inclin_delta,
    _decode_inclin_delta_v1,
    _decode_temp_abs,
    _decode_temp_delta,
    _decode_temp_delta_v1,
    FORMAT_V1,
    FORMAT_V2,
    FLAG_ACCEL,
    FLAG_INCLIN,
    FLAG_TEMP,
    SENTINEL,
)


@contextmanager
def open_record_stream(filepath: str):
    """
    Plotting decoder file opener that keeps memory usage low.

    .bin:
      - use the file directly

    .bin.gz:
      - stream-decompress using gzip.open(..., "rb")
      - wrap in BufferedReader so the existing binary decode helpers can
        continue using read()/struct unpack operations efficiently

    This keeps plotting compatible with both active .bin files and finalized
    .bin.gz files without loading the full decompressed file into RAM.
    """
    with ExitStack() as stack:
        if filepath.endswith(".gz"):
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
    Streaming decoder used by the sensor plotting endpoints.

    This function still decodes one record at a time so plotting can work with
    both .bin and .bin.gz files while keeping memory use low.

    Note:
    Export no longer uses this decoder path. Raw export now packages backend
    storage files directly into a ZIP archive.
    """
    state = _fresh_decode_state()

    with open_record_stream(filepath) as f:
        ver = f.read(1)
        if not ver:
            return

        fv = ver[0]
        if fv not in (FORMAT_V1, FORMAT_V2):
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
                    else:
                        if header & FLAG_ACCEL:
                            rec["accel_samples"] = _decode_accel_delta(f, state)
                        if header & FLAG_INCLIN:
                            rec["inclin"] = _decode_inclin_delta(f, state)
                        if header & FLAG_TEMP:
                            rec["temp"] = _decode_temp_delta(f, state)

                yield rec
                idx += 1

            except EOFError:
                # Current-hour .bin can be actively written and end mid-record.
                break
            except struct.error:
                break