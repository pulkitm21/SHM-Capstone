"""
raw_backup.py
-------------
Writes every raw MQTT payload verbatim to a .jsonl.zst file the moment
it arrives — BEFORE any timestamp normalisation, validation, or delta
encoding.  This preserves exactly what the sensor sent, including
packets that will later be dropped by the encoder (bad clock, missing
fields, malformed JSON, etc).

File layout:
    RAW_DIR/<node_id>/<node_id>_YYYYMMDD_HH.jsonl.zst

Each line is the raw JSON payload followed by a newline.  The file uses
Zstd streaming compression with FLUSH_BLOCK after every write, so data
is durable without closing the file — at most one packet is lost on a
hard power cut.

Usage (called from delta_encoder.py):
    from raw_backup import write_raw
    write_raw(node_id, msg.payload)   # bytes, called in on_message
"""

import os
import threading
from datetime import datetime

import zstandard as zstd

# -------------------------------------------------------------------
# Configuration — must match delta_encoder.py directory layout
# -------------------------------------------------------------------
RAW_DIR    = "/mnt/ssd/raw"
ZSTD_LEVEL = 3       # 1 (fastest) … 22 (smallest); 3 is a good balance

os.makedirs(RAW_DIR, exist_ok=True)

# -------------------------------------------------------------------
# Internal state
#
# _handles[node_id] = {
#   "hour":   str,                  # "YYYYMMDD_HH" of the open file
#   "writer": ZstdCompressionWriter, # streaming compressor handle
#   "file":   file object,           # underlying .jsonl.zst file
# }
# -------------------------------------------------------------------
_handles: dict  = {}
_lock           = threading.Lock()   # guards _handles across threads


# -------------------------------------------------------------------
# Internal helpers
# -------------------------------------------------------------------

def _open_writer(node_id: str, hour_str: str) -> dict:
    """Open a new .jsonl.zst file and return a handle dict."""
    node_dir = os.path.join(RAW_DIR, node_id)
    os.makedirs(node_dir, exist_ok=True)
    path   = os.path.join(node_dir, f"{node_id}_{hour_str}.jsonl.zst")
    f      = open(path, "ab")
    cctx   = zstd.ZstdCompressor(level=ZSTD_LEVEL)
    writer = cctx.stream_writer(f, closefd=False)
    print(f"[raw_backup] [{node_id}] Opened {path}")
    return {"hour": hour_str, "writer": writer, "file": f}


def _close_handle(node_id: str, handle: dict):
    """Flush and close an open handle.  Errors are logged, not raised."""
    try:
        handle["writer"].__exit__(None, None, None)
        handle["file"].close()
    except Exception as e:
        print(f"[raw_backup] [{node_id}] Warning: error closing file: {e}")


def _get_writer(node_id: str, hour_str: str):
    """
    Return the active streaming writer for this node+hour.
    Opens a new file (closing the previous one) when the hour changes.
    Thread-safe.
    """
    with _lock:
        handle = _handles.get(node_id)

        if handle and handle["hour"] == hour_str:
            return handle["writer"]

        # Hour rolled over or first packet for this node
        if handle:
            _close_handle(node_id, handle)

        handle = _open_writer(node_id, hour_str)
        _handles[node_id] = handle
        return handle["writer"]


# -------------------------------------------------------------------
# Public API
# -------------------------------------------------------------------

def write_raw(node_id: str, payload: bytes) -> None:
    """
    Append one raw MQTT payload as a JSONL line to the backup file.

    Parameters
    ----------
    node_id : str
        Sensor node identifier (e.g. "N01").
    payload : bytes
        The exact bytes from msg.payload — written before any decoding
        or validation so that dropped packets are still captured.
    """
    hour_str = datetime.now().strftime("%Y%m%d_%H")
    writer   = _get_writer(node_id, hour_str)
    line     = payload if payload.endswith(b"\n") else payload + b"\n"
    try:
        writer.write(line)
        writer.flush(zstd.FLUSH_BLOCK)   # durable: survives a hard power cut
    except Exception as e:
        print(f"[raw_backup] [{node_id}] Warning: write failed: {e}")


def close_all() -> None:
    """
    Flush and close all open raw backup files.
    Call this on clean shutdown to avoid a truncated final Zstd frame.
    """
    with _lock:
        for node_id, handle in list(_handles.items()):
            _close_handle(node_id, handle)
        _handles.clear()
    print("[raw_backup] All handles closed.")
