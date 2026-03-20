# Handles inserting fault events into faults.db

import sqlite3
from pathlib import Path
from typing import Iterable, Any

from fault_codes import get_fault_definition


FAULT_DB_PATH = Path("/mnt/ssd/fault/faults.db")


def log_fault_codes(
    serial_number: str,
    fault_codes: Iterable[int],
    mqtt_ts: Any,
):
    """
    Insert multiple fault codes from a single MQTT packet.

    The timestamp is stored exactly as received from the MQTT packet.
    """
    unique_codes = list(dict.fromkeys(int(c) for c in fault_codes))
    if not unique_codes:
        return

    ts = str(mqtt_ts)

    with sqlite3.connect(FAULT_DB_PATH) as conn:
        for code in unique_codes:
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