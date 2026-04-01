"""
raw_backup_binary.py
--------------------
Writes every raw MQTT payload verbatim to an append-only binary file the
moment it arrives — BEFORE any timestamp normalisation, validation, or
delta encoding. This preserves exactly what the sensor sent, including
packets that will later be dropped by the encoder (bad clock, missing
fields, malformed JSON, etc).

File layout:
    RAW_DIR/<node_id>/<node_id>_YYYYMMDD_HH.rawbin

Each record is length-prefixed so the file is replayable without relying
on newlines or text parsing.

Record format (little-endian):
    [8 bytes recv_unix_ns][4 bytes payload_len][payload bytes]

That means every append is self-delimiting:
- recv_unix_ns : wall-clock receive time on the Pi in Unix nanoseconds
- payload_len  : number of bytes in the MQTT payload
- payload      : exact raw msg.payload bytes

Usage (called from delta_encoder.py):
    from raw_backup_binary import write_raw
    write_raw(node_id, msg.payload)   # bytes, called in on_message
"""

import os
import struct
import threading
from datetime import datetime, timedelta
from time import time_ns, monotonic

# -------------------------------------------------------------------
# Configuration — must match delta_encoder.py directory layout
# -------------------------------------------------------------------
RAW_DIR = "/mnt/ssd/raw"

os.makedirs(RAW_DIR, exist_ok=True)

# Retain raw backup files for roughly 2 years.
RAW_RETENTION_DAYS = 730

# Run cleanup periodically rather than on every packet.
_CLEANUP_INTERVAL_S = 24 * 3600  # every 24 hours
_last_cleanup_monotonic = 0.0

# -------------------------------------------------------------------
# Internal state
#
# _handles[node_id] = {
#   "hour": str,      # "YYYYMMDD_HH" of the open file
#   "file": file obj, # underlying .rawbin file
# }
# -------------------------------------------------------------------
_handles: dict = {}
_lock = threading.Lock()  # guards _handles across threads

# Record header: uint64 receive time (ns), uint32 payload length
_HEADER_STRUCT = struct.Struct("<QI")


# -------------------------------------------------------------------
# Internal helpers
# -------------------------------------------------------------------

def _open_file(node_id: str, hour_str: str) -> dict:
    """Open a new append-only .rawbin file and return a handle dict."""
    node_dir = os.path.join(RAW_DIR, node_id)
    os.makedirs(node_dir, exist_ok=True)
    path = os.path.join(node_dir, f"{node_id}_{hour_str}.rawbin")
    f = open(path, "ab", buffering=0)  # append-only, unbuffered binary writes
    print(f"[raw_backup_binary] [{node_id}] Opened {path}")
    return {"hour": hour_str, "file": f}


def _close_handle(node_id: str, handle: dict):
    """Flush and close an open handle. Errors are logged, not raised."""
    try:
        handle["file"].flush()
        os.fsync(handle["file"].fileno())
        handle["file"].close()
    except Exception as e:
        print(f"[raw_backup_binary] [{node_id}] Warning: error closing file: {e}")


def _get_file(node_id: str, hour_str: str):
    """
    Return the active file handle for this node+hour.
    Opens a new file (closing the previous one) when the hour changes.
    Thread-safe.
    """
    with _lock:
        handle = _handles.get(node_id)

        if handle and handle["hour"] == hour_str:
            return handle["file"]

        # Hour rolled over or first packet for this node
        if handle:
            _close_handle(node_id, handle)

        handle = _open_file(node_id, hour_str)
        _handles[node_id] = handle
        return handle["file"]


def _parse_rawbin_hour_from_name(filename: str) -> datetime | None:
    """
    Parse <node_id>_YYYYMMDD_HH.rawbin and return the file hour as datetime.
    Returns None if the filename does not match the expected pattern.
    """
    if not filename.endswith(".rawbin"):
        return None

    stem = filename[:-7]  # remove ".rawbin"
    try:
        # Split from the right so node_id may contain underscores safely.
        _, hour_str = stem.rsplit("_", 1)
        # hour_str currently only has HH if we split once; need last 2 parts
        parts = stem.rsplit("_", 2)
        if len(parts) < 3:
            return None
        dt_str = f"{parts[-2]}_{parts[-1]}"
        return datetime.strptime(dt_str, "%Y%m%d_%H")
    except Exception:
        return None


def _cleanup_old_raw_files(force: bool = False) -> None:
    """
    Delete raw backup files older than the retention window.
    Skips files that are currently open.
    """
    global _last_cleanup_monotonic

    now_mono = monotonic()
    if not force and (now_mono - _last_cleanup_monotonic) < _CLEANUP_INTERVAL_S:
        return

    cutoff = datetime.now() - timedelta(days=RAW_RETENTION_DAYS)

    with _lock:
        open_paths = {
            os.path.abspath(handle["file"].name)
            for handle in _handles.values()
            if "file" in handle and not handle["file"].closed
        }

        try:
            node_dirs = list(os.scandir(RAW_DIR))
        except FileNotFoundError:
            _last_cleanup_monotonic = now_mono
            return
        except Exception as e:
            print(f"[raw_backup_binary] Warning: cleanup scan failed: {e}")
            _last_cleanup_monotonic = now_mono
            return

        deleted = 0

        for node_entry in node_dirs:
            if not node_entry.is_dir():
                continue

            try:
                file_entries = list(os.scandir(node_entry.path))
            except Exception as e:
                print(f"[raw_backup_binary] Warning: cleanup scan failed for {node_entry.path}: {e}")
                continue

            for file_entry in file_entries:
                if not file_entry.is_file():
                    continue

                file_dt = _parse_rawbin_hour_from_name(file_entry.name)
                if file_dt is None:
                    continue
                if file_dt >= cutoff:
                    continue

                abs_path = os.path.abspath(file_entry.path)
                if abs_path in open_paths:
                    continue

                try:
                    os.remove(file_entry.path)
                    deleted += 1
                except Exception as e:
                    print(f"[raw_backup_binary] Warning: failed to delete old raw backup {file_entry.path}: {e}")

        if deleted:
            print(
                f"[raw_backup_binary] Cleanup removed {deleted} raw backup file(s) "
                f"older than {RAW_RETENTION_DAYS} days."
            )

    _last_cleanup_monotonic = now_mono


# -------------------------------------------------------------------
# Public API
# -------------------------------------------------------------------

def write_raw(node_id: str, payload: bytes) -> None:
    """
    Append one raw MQTT payload as a framed binary record.

    Parameters
    ----------
    node_id : str
        Sensor node identifier (e.g. "N01").
    payload : bytes
        The exact bytes from msg.payload — written before any decoding
        or validation so that dropped packets are still captured.
    """
    if not isinstance(payload, (bytes, bytearray, memoryview)):
        raise TypeError("payload must be bytes-like")

    payload = bytes(payload)
    recv_ns = time_ns()
    hour_str = datetime.now().strftime("%Y%m%d_%H")
    f = _get_file(node_id, hour_str)
    header = _HEADER_STRUCT.pack(recv_ns, len(payload))

    try:
        f.write(header)
        f.write(payload)
        f.flush()
        os.fsync(f.fileno())  # durable: survives restart/power loss after write returns
    except Exception as e:
        print(f"[raw_backup_binary] [{node_id}] Warning: write failed: {e}")

    # Periodic retention cleanup.
    _cleanup_old_raw_files()


def close_all() -> None:
    """
    Flush and close all open raw backup files.
    Call this on clean shutdown.
    """
    with _lock:
        for node_id, handle in list(_handles.items()):
            _close_handle(node_id, handle)
        _handles.clear()
    print("[raw_backup_binary] All handles closed.")


# -------------------------------------------------------------------
# Optional helper for replay / inspection
# -------------------------------------------------------------------

def iter_records(path: str):
    """
    Iterate over records in a .rawbin file.

    Yields
    ------
    tuple[int, bytes]
        (recv_unix_ns, payload_bytes)
    """
    with open(path, "rb") as f:
        while True:
            header = f.read(_HEADER_STRUCT.size)
            if not header:
                return
            if len(header) != _HEADER_STRUCT.size:
                raise IOError(f"Truncated record header in {path}")

            recv_ns, payload_len = _HEADER_STRUCT.unpack(header)
            payload = f.read(payload_len)
            if len(payload) != payload_len:
                raise IOError(f"Truncated payload in {path}: expected {payload_len} bytes")

            yield recv_ns, payload