from __future__ import annotations

import csv
import os
import re
import tempfile
from datetime import date, datetime, time, timedelta, timezone
from pathlib import Path
from typing import Any, Iterable, Optional


DATA_DIR = Path("/mnt/ssd")
FAULTS_DB = DATA_DIR / "fault" / "faults.db"
EXPORT_TMP_DIR = Path("/tmp/shm_exports")

FAULT_EXPORT_HEADERS = [
    "id",
    "serial_number",
    "sensor_type",
    "fault_type",
    "severity",
    "fault_status",
    "description",
    "ts",
]


def is_ssd_available() -> bool:
    return bool(
        DATA_DIR.exists()
        and os.path.ismount(DATA_DIR)
        and os.access(DATA_DIR, os.R_OK)
    )


def is_fault_db_available() -> bool:
    return is_ssd_available() and FAULTS_DB.exists()


def normalize_fault_text(value: Optional[str]) -> str:
    return str(value or "").strip()


def sanitize_filename_part(value: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9._-]+", "_", value.strip())
    cleaned = cleaned.strip("._-")
    return cleaned or "all"


def parse_day_string(value: str) -> date:
    try:
        return datetime.strptime(value, "%Y-%m-%d").date()
    except ValueError as exc:
        raise ValueError(f"Invalid day value '{value}'. Expected YYYY-MM-DD.") from exc


def day_start_iso(value: str) -> str:
    day = parse_day_string(value)
    dt = datetime.combine(day, time.min, tzinfo=timezone.utc)
    return dt.isoformat()


def day_end_exclusive_iso(value: str) -> str:
    day = parse_day_string(value) + timedelta(days=1)
    dt = datetime.combine(day, time.min, tzinfo=timezone.utc)
    return dt.isoformat()


def validate_day_range(
    start_day: Optional[str],
    end_day: Optional[str],
) -> tuple[Optional[str], Optional[str]]:
    if bool(start_day) != bool(end_day):
        raise ValueError("start_day and end_day must both be provided together.")

    if not start_day or not end_day:
        return None, None

    start_date = parse_day_string(start_day)
    end_date = parse_day_string(end_day)

    if end_date < start_date:
        raise ValueError("end_day cannot be earlier than start_day.")

    return day_start_iso(start_day), day_end_exclusive_iso(end_day)


def build_fault_export_where(
    *,
    start_day: Optional[str] = None,
    end_day: Optional[str] = None,
    serial_number: Optional[str] = None,
    sensor_type: Optional[str] = None,
    fault_type: Optional[str] = None,
    severity: Optional[int] = None,
    fault_status: Optional[str] = None,
    description: Optional[str] = None,
) -> tuple[list[str], list[Any]]:
    where_clauses: list[str] = []
    params: list[Any] = []

    start_iso, end_exclusive_iso = validate_day_range(start_day, end_day)

    serial_value = normalize_fault_text(serial_number)
    sensor_value = normalize_fault_text(sensor_type)
    fault_type_value = normalize_fault_text(fault_type)
    status_value = normalize_fault_text(fault_status)
    description_value = normalize_fault_text(description)

    if start_iso:
        where_clauses.append("ts >= ?")
        params.append(start_iso)

    if end_exclusive_iso:
        where_clauses.append("ts < ?")
        params.append(end_exclusive_iso)

    if serial_value:
        where_clauses.append("serial_number LIKE ?")
        params.append(f"%{serial_value}%")

    if sensor_value:
        where_clauses.append("LOWER(sensor_type) = LOWER(?)")
        params.append(sensor_value)

    if fault_type_value:
        where_clauses.append("LOWER(fault_type) = LOWER(?)")
        params.append(fault_type_value)

    if severity is not None:
        where_clauses.append("severity = ?")
        params.append(severity)

    if status_value:
        where_clauses.append("LOWER(fault_status) = LOWER(?)")
        params.append(status_value)

    if description_value:
        where_clauses.append("description LIKE ?")
        params.append(f"%{description_value}%")

    return where_clauses, params


def build_where_sql(where_clauses: list[str]) -> str:
    if not where_clauses:
        return ""
    return "WHERE " + " AND ".join(where_clauses)


def ensure_export_tmp_dir() -> None:
    EXPORT_TMP_DIR.mkdir(parents=True, exist_ok=True)


def create_temp_export_path(prefix: str, suffix: str = ".csv") -> Path:
    ensure_export_tmp_dir()
    fd, path = tempfile.mkstemp(
        prefix=prefix,
        suffix=suffix,
        dir=str(EXPORT_TMP_DIR),
    )
    os.close(fd)
    return Path(path)


def remove_temp_file(path: Path) -> None:
    try:
        path.unlink(missing_ok=True)
    except OSError:
        pass


def write_fault_csv(
    export_path: Path,
    rows: Iterable[dict[str, Any]],
) -> None:
    with export_path.open("w", newline="", encoding="utf-8") as csv_file:
        writer = csv.writer(csv_file)
        writer.writerow(FAULT_EXPORT_HEADERS)

        for row in rows:
            writer.writerow(
                [
                    row.get("id", ""),
                    row.get("serial_number", ""),
                    row.get("sensor_type", ""),
                    row.get("fault_type", ""),
                    row.get("severity", ""),
                    row.get("fault_status", ""),
                    row.get("description", ""),
                    row.get("ts", ""),
                ]
            )


def build_fault_export_filename(
    *,
    start_day: Optional[str],
    end_day: Optional[str],
    serial_number: Optional[str],
) -> str:
    if start_day and end_day:
        date_part = f"{start_day}_to_{end_day}"
    else:
        date_part = "full_log"

    serial_part = sanitize_filename_part(serial_number or "all_nodes")
    return f"fault_export_{serial_part}_{date_part}.csv"