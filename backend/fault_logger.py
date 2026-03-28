import os
import sqlite3
from pathlib import Path
from typing import Iterable, Any

from fault_codes import get_fault_definition

FAULT_DB_PATH = Path("/mnt/ssd/fault/faults.db")
SSD_ROOT = Path("/mnt/ssd")

# Stateful fault types that should be reconciled into current fault state.
STATEFUL_FAULT_TYPES = {
    "ethernet_link",
    "mqtt_connection",
    "power_loss",
}


def _ensure_fault_table(conn: sqlite3.Connection) -> None:
    # Create the faults table when the DB is empty.
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS faults (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            serial_number TEXT,
            fault_code INTEGER,
            sensor_type TEXT,
            fault_type TEXT,
            state_key TEXT,
            is_stateful INTEGER,
            severity INTEGER,
            fault_status TEXT,
            description TEXT,
            ts TEXT
        )
        """
    )


def _get_existing_columns(conn: sqlite3.Connection) -> set[str]:
    # Read current table columns for lightweight schema migration.
    cur = conn.execute("PRAGMA table_info(faults)")
    return {str(row[1]) for row in cur.fetchall()}


def _ensure_fault_columns(conn: sqlite3.Connection) -> None:
    # Add new columns for state-based fault tracking.
    existing = _get_existing_columns(conn)

    if "fault_code" not in existing:
        conn.execute("ALTER TABLE faults ADD COLUMN fault_code INTEGER")

    if "state_key" not in existing:
        conn.execute("ALTER TABLE faults ADD COLUMN state_key TEXT")

    if "is_stateful" not in existing:
        conn.execute("ALTER TABLE faults ADD COLUMN is_stateful INTEGER")


def _backfill_stateful_metadata(conn: sqlite3.Connection) -> None:
    # Backfill legacy rows so active fault queries work with older data.
    placeholders = ",".join("?" for _ in STATEFUL_FAULT_TYPES)

    conn.execute(
        f"""
        UPDATE faults
        SET
            state_key = fault_type,
            is_stateful = 1
        WHERE fault_type IN ({placeholders})
          AND (state_key IS NULL OR TRIM(state_key) = '' OR is_stateful IS NULL)
        """,
        tuple(STATEFUL_FAULT_TYPES),
    )

    conn.execute(
        f"""
        UPDATE faults
        SET is_stateful = 0
        WHERE fault_type NOT IN ({placeholders})
          AND is_stateful IS NULL
        """,
        tuple(STATEFUL_FAULT_TYPES),
    )


def ensure_fault_db_schema() -> None:
    # Keep the DB schema compatible with current fault-state logic.
    if not SSD_ROOT.exists() or not os.path.ismount(SSD_ROOT):
        return

    FAULT_DB_PATH.parent.mkdir(parents=True, exist_ok=True)

    conn = None
    try:
        conn = sqlite3.connect(FAULT_DB_PATH)
        _ensure_fault_table(conn)
        _ensure_fault_columns(conn)
        _backfill_stateful_metadata(conn)
        conn.commit()
    except Exception as e:
        print(f"[fault_logger] Failed to ensure fault DB schema: {e}")
    finally:
        if conn is not None:
            conn.close()


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

    FAULT_DB_PATH.parent.mkdir(parents=True, exist_ok=True)

    conn = None
    try:
        conn = sqlite3.connect(FAULT_DB_PATH)
        _ensure_fault_table(conn)
        _ensure_fault_columns(conn)

        for code, ts in unique_events:
            f = get_fault_definition(code)

            conn.execute(
                """
                INSERT INTO faults (
                    serial_number,
                    fault_code,
                    sensor_type,
                    fault_type,
                    state_key,
                    is_stateful,
                    severity,
                    fault_status,
                    description,
                    ts
                )
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    serial_number,
                    code,
                    f.sensor_type,
                    f.fault_type,
                    f.state_key,
                    int(f.is_stateful),
                    f.severity,
                    f.fault_status,
                    f.description,
                    ts,
                ),
            )

        conn.commit()
    except Exception as e:
        print(f"[fault_logger] Failed to write fault log: {e}")
    finally:
        if conn is not None:
            conn.close()