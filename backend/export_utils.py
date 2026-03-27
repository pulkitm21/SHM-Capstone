from __future__ import annotations

import csv
import os
import re
import shutil
import tempfile
import zipfile
from datetime import date, datetime, time, timedelta, timezone
from pathlib import Path
from typing import Any, Iterable, Optional

DATA_DIR = Path("/mnt/ssd")
FAULTS_DB = DATA_DIR / "fault" / "faults.db"
SENSOR_DATA_DIR = DATA_DIR / "data"
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


def is_sensor_data_available() -> bool:
    return is_ssd_available() and SENSOR_DATA_DIR.exists() and SENSOR_DATA_DIR.is_dir()


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


def parse_optional_hour_string(value: Optional[str], *, field_name: str) -> Optional[int]:
    if value is None or str(value).strip() == "":
        return None

    try:
        parsed = int(str(value).strip())
    except ValueError as exc:
        raise ValueError(f"Invalid {field_name} value '{value}'. Expected 00-23.") from exc

    if parsed < 0 or parsed > 23:
        raise ValueError(f"Invalid {field_name} value '{value}'. Expected 00-23.")

    return parsed


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


def validate_required_day_range(
    start_day: Optional[str],
    end_day: Optional[str],
) -> tuple[str, str]:
    start_iso, end_exclusive_iso = validate_day_range(start_day, end_day)
    if not start_iso or not end_exclusive_iso:
        raise ValueError("start_day and end_day are required.")
    return start_iso, end_exclusive_iso


def validate_required_day_hour_range(
    *,
    start_day: Optional[str],
    end_day: Optional[str],
    start_hour: Optional[str],
    end_hour: Optional[str],
) -> tuple[str, str, Optional[int], Optional[int]]:
    if bool(start_day) != bool(end_day):
        raise ValueError("start_day and end_day must both be provided together.")

    if not start_day or not end_day:
        raise ValueError("start_day and end_day are required.")

    start_date = parse_day_string(start_day)
    end_date = parse_day_string(end_day)

    if end_date < start_date:
        raise ValueError("end_day cannot be earlier than start_day.")

    parsed_start_hour = parse_optional_hour_string(start_hour, field_name="start_hour")
    parsed_end_hour = parse_optional_hour_string(end_hour, field_name="end_hour")

    if (parsed_start_hour is None) != (parsed_end_hour is None):
        raise ValueError("start_hour and end_hour must both be provided together.")

    if parsed_start_hour is None and parsed_end_hour is None:
        start_dt = datetime.combine(start_date, time.min, tzinfo=timezone.utc)
        end_dt = datetime.combine(end_date + timedelta(days=1), time.min, tzinfo=timezone.utc)
        return start_dt.isoformat(), end_dt.isoformat(), None, None

    start_dt = datetime.combine(
        start_date,
        time(hour=parsed_start_hour),
        tzinfo=timezone.utc,
    )
    end_dt = datetime.combine(
        end_date,
        time(hour=parsed_end_hour),
        tzinfo=timezone.utc,
    ) + timedelta(hours=1)

    if end_dt <= start_dt:
        raise ValueError("End hour cannot be earlier than start hour for the selected range.")

    return start_dt.isoformat(), end_dt.isoformat(), parsed_start_hour, parsed_end_hour


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


def create_temp_export_dir(prefix: str) -> Path:
    ensure_export_tmp_dir()
    return Path(tempfile.mkdtemp(prefix=prefix, dir=str(EXPORT_TMP_DIR)))


def remove_temp_file(path: Path) -> None:
    try:
        path.unlink(missing_ok=True)
    except OSError:
        pass


def remove_temp_dir(path: Path) -> None:
    if not path.exists():
        return

    for child in path.iterdir():
        if child.is_file():
            remove_temp_file(child)
        elif child.is_dir():
            remove_temp_dir(child)

    try:
        path.rmdir()
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


def parse_node_ids_csv(value: str) -> list[int]:
    parts = [part.strip() for part in value.split(",")]
    out: list[int] = []

    for part in parts:
        if not part:
            continue
        try:
            out.append(int(part))
        except ValueError as exc:
            raise ValueError(f"Invalid node id '{part}'.") from exc

    unique_sorted = sorted(set(out))
    if not unique_sorted:
        raise ValueError("At least one node id is required.")

    return unique_sorted


def parse_iso_to_epoch_seconds(value: str) -> float:
    return datetime.fromisoformat(value).timestamp()


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def iter_hour_starts(start_iso: str, end_exclusive_iso: str):
    start_dt = datetime.fromisoformat(start_iso).astimezone(timezone.utc)
    end_dt = datetime.fromisoformat(end_exclusive_iso).astimezone(timezone.utc)

    current = start_dt.replace(minute=0, second=0, microsecond=0)

    while current < end_dt:
        yield current
        current += timedelta(hours=1)


def find_sensor_files_for_serial(
    serial: str,
    start_iso: str,
    end_exclusive_iso: str,
) -> list[Path]:
    """
    Only return files whose hourly bucket overlaps the requested time range.

    Prefer .bin for the active/current hour if both .bin and .bin.gz somehow
    exist at the same time.
    """
    matched: list[Path] = []

    for hour_dt in iter_hour_starts(start_iso, end_exclusive_iso):
        hour_str = hour_dt.strftime("%Y%m%d_%H")
        base = f"data_{serial}_{hour_str}"
        bin_path = SENSOR_DATA_DIR / f"{base}.bin"
        gz_path = SENSOR_DATA_DIR / f"{base}.bin.gz"

        if bin_path.exists():
            matched.append(bin_path)
        elif gz_path.exists():
            matched.append(gz_path)

    return matched


def build_sensor_scope_filename_part(node_ids: list[int]) -> str:
    if len(node_ids) == 1:
        return f"node_{node_ids[0]}"
    joined_ids = "-".join(str(node_id) for node_id in node_ids)
    return f"nodes_{joined_ids}"


def build_sensor_range_filename_part(day_value: str, hour_value: Optional[int]) -> str:
    if hour_value is None:
        return day_value
    return f"{day_value}_{hour_value:02d}"


def build_sensor_export_zip_filename(
    *,
    start_day: str,
    end_day: str,
    node_ids: list[int],
    start_hour: Optional[int] = None,
    end_hour: Optional[int] = None,
) -> str:
    scope = build_sensor_scope_filename_part(node_ids)
    start_part = build_sensor_range_filename_part(start_day, start_hour)
    end_part = build_sensor_range_filename_part(end_day, end_hour)
    return f"{scope}_{start_part}_{end_part}.zip"


def build_raw_sensor_export_metadata_text(
    *,
    generated_at: str,
    start_day: str,
    end_day: str,
    start_hour: Optional[int],
    end_hour: Optional[int],
    nodes: list[dict[str, Any]],
    node_file_counts: dict[str, int],
    exported_files: dict[str, list[str]],
) -> str:
    lines: list[str] = []

    lines.append("SHM Raw Sensor File Export Metadata")
    lines.append("=" * 35)
    lines.append(f"generated_at_utc: {generated_at}")
    lines.append(f"start_day: {start_day}")
    lines.append(f"end_day: {end_day}")
    lines.append(f"start_hour_utc: {'' if start_hour is None else f'{start_hour:02d}'}")
    lines.append(f"end_hour_utc: {'' if end_hour is None else f'{end_hour:02d}'}")
    lines.append(f"node_count: {len(nodes)}")
    lines.append("")

    lines.append("Selected Nodes")
    lines.append("-" * 14)
    for node in nodes:
        node_id = node.get("node_id")
        serial = node.get("serial")
        label = node.get("label") or f"Node {node_id}"
        lines.append(f"node_id={node_id}, label={label}, serial={serial}")
    lines.append("")

    lines.append("Matched Hourly Files")
    lines.append("-" * 20)
    for node in nodes:
        serial = str(node.get("serial"))
        file_count = node_file_counts.get(serial, 0)
        lines.append(f"{serial}: {file_count} matching file(s)")
        for file_name in exported_files.get(serial, []):
            lines.append(f"  - {file_name}")
    lines.append("")

    lines.append("Notes")
    lines.append("-" * 5)
    lines.append("This export contains raw hourly backend storage files.")
    lines.append("Files are copied into this ZIP archive with user-facing export names.")
    lines.append("Current hour files may appear as .bin.")
    lines.append("Finalized historical files may appear as .bin.gz.")
    lines.append("No decode or CSV conversion was performed by the backend.")

    return "\n".join(lines) + "\n"


def build_export_member_name(
    *,
    node_id: int,
    bucket_day: str,
    bucket_hour: str,
    source_name: str,
) -> str:
    if source_name.endswith(".bin.gz"):
        suffix = ".bin.gz"
    else:
        suffix = ".bin"
    return f"node_{node_id}_{bucket_day}_{bucket_hour}{suffix}"


def extract_sensor_bucket_from_filename(source_name: str) -> tuple[str, str]:
    match = re.match(r"^data_.+_(\d{8})_(\d{2})\.bin(?:\.gz)?$", source_name)
    if not match:
        raise ValueError(f"Unrecognized sensor export source filename: {source_name}")

    day_token = match.group(1)
    hour_token = match.group(2)
    bucket_day = f"{day_token[0:4]}-{day_token[4:6]}-{day_token[6:8]}"
    return bucket_day, hour_token


def stage_files_for_zip(
    export_dir: Path,
    members: list[tuple[Path, str]],
) -> list[Path]:
    """
    Copy matching storage files into the temporary export directory before zipping.
    This keeps the ZIP response stable even if source files change during download.
    """
    staged_paths: list[Path] = []

    for source_path, destination_name in members:
        destination = export_dir / destination_name
        shutil.copy2(source_path, destination)
        staged_paths.append(destination)

    return staged_paths


def create_zip_from_paths(zip_path: Path, members: list[Path]) -> None:
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for member in members:
            zf.write(member, arcname=member.name)


def write_text_file(path: Path, content: str) -> None:
    path.write_text(content, encoding="utf-8")
