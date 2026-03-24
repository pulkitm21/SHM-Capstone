import os
import sqlite3
from pathlib import Path
from typing import Iterable, Any

from fault_codes import get_fault_definition

FAULT_DB_PATH = Path("/mnt/ssd/fault/faults.db")
SSD_ROOT = Path("/mnt/ssd")


def log_fault_events(
    serial_number: str,
    fault_events: Iterable[tuple[int, Any]],
) -> None:
    # Remove duplicate fault code + timestamp pairs from the same packet.
    unique_events = list(dict.fromkeys((int(code), str(ts)) for code, ts in fault_events))
    if not unique_events:
        return

    # Skip fault logging if the SSD is not mounted.
    if not SSD_ROOT.exists() or not os.path.ismount(SSD_ROOT):
        print("[fault_logger] SSD not mounted. Skipping fault log.")
        return

    try:
        with sqlite3.connect(FAULT_DB_PATH) as conn:
            for code, ts in unique_events:
                f = get_fault_definition(code)

                conn.execute(
                    """
                    INSERT INTO faults (
                        serial_number,
                        sensor_type,
                        fault_type,
                        severity,
                        fault_status,
                        description,
                        ts
                    )
                    VALUES (?, ?, ?, ?, ?, ?, ?)
                    """,
                    (
                        serial_number,
                        f.sensor_type,
                        f.fault_type,
                        f.severity,
                        f.fault_status,
                        f.description,
                        ts,
                    ),
                )

            conn.commit()
    except Exception as e:
        print(f"[fault_logger] Failed to write fault log: {e}")