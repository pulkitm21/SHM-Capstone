from __future__ import annotations

import sqlite3
from pathlib import Path
from typing import Optional

from fastapi import APIRouter, HTTPException, Query
from fastapi.responses import FileResponse
from starlette.background import BackgroundTask

from export_utils import (
    FAULTS_DB,
    build_fault_export_filename,
    build_fault_export_where,
    build_raw_sensor_export_metadata_text,
    build_sensor_export_zip_filename,
    build_where_sql,
    create_temp_export_dir,
    create_temp_export_path,
    create_zip_from_paths,
    find_sensor_files_for_serial,
    is_fault_db_available,
    is_sensor_data_available,
    parse_node_ids_csv,
    remove_temp_dir,
    remove_temp_file,
    stage_files_for_zip,
    utc_now_iso,
    validate_required_day_range,
    write_fault_csv,
    write_text_file,
)
from node_registry import get_node_by_id

router = APIRouter()


def _cleanup_sensor_export(export_dir: Path, zip_path: Path) -> None:
    remove_temp_file(zip_path)
    remove_temp_dir(export_dir)


@router.get("/api/exports/faults")
def export_faults(
    start_day: Optional[str] = Query(default=None),
    end_day: Optional[str] = Query(default=None),
    serial_number: Optional[str] = Query(default=None),
    sensor_type: Optional[str] = Query(default=None),
    fault_type: Optional[str] = Query(default=None),
    severity: Optional[int] = Query(default=None),
    fault_status: Optional[str] = Query(default=None),
    description: Optional[str] = Query(default=None),
):
    if not is_fault_db_available():
        raise HTTPException(
            status_code=503,
            detail="Fault export unavailable because the SSD or faults database is not available.",
        )

    try:
        where_clauses, where_params = build_fault_export_where(
            start_day=start_day,
            end_day=end_day,
            serial_number=serial_number,
            sensor_type=sensor_type,
            fault_type=fault_type,
            severity=severity,
            fault_status=fault_status,
            description=description,
        )
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc

    where_sql = build_where_sql(where_clauses)
    export_path = create_temp_export_path(prefix="fault_export_")
    filename = build_fault_export_filename(
        start_day=start_day,
        end_day=end_day,
        serial_number=serial_number,
    )

    con: Optional[sqlite3.Connection] = None

    try:
        con = sqlite3.connect(str(FAULTS_DB))
        con.row_factory = sqlite3.Row
        cur = con.cursor()

        cur.execute(
            f"""
            SELECT id, serial_number, sensor_type, fault_type, severity, fault_status, description, ts
            FROM faults
            {where_sql}
            ORDER BY ts DESC
            """,
            where_params,
        )

        def iter_rows():
            while True:
                batch = cur.fetchmany(1000)
                if not batch:
                    break
                for row in batch:
                    yield dict(row)

        write_fault_csv(export_path=export_path, rows=iter_rows())

    except sqlite3.Error as exc:
        remove_temp_file(export_path)
        raise HTTPException(
            status_code=500,
            detail=f"Failed to export fault log: {exc}",
        ) from exc

    finally:
        if con is not None:
            con.close()

    return FileResponse(
        path=str(export_path),
        media_type="text/csv",
        filename=filename,
        background=BackgroundTask(remove_temp_file, export_path),
    )


@router.get("/api/exports/sensor-data")
def export_sensor_data(
    node_ids: str = Query(..., description="Comma-separated node ids, e.g. 1,2,3"),
    start_day: str = Query(...),
    end_day: str = Query(...),
):
    """
    Export raw hourly sensor storage files for the selected nodes and day range.

    Output format:
    - always returns a ZIP
    - contains the matching .bin and/or .bin.gz files
    - includes export_metadata.txt
    """
    if not is_sensor_data_available():
        raise HTTPException(
            status_code=503,
            detail="Sensor export unavailable because the SSD or sensor data directory is not available.",
        )

    try:
        parsed_node_ids = parse_node_ids_csv(node_ids)
        start_iso, end_exclusive_iso = validate_required_day_range(start_day, end_day)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc

    resolved_nodes: list[dict[str, object]] = []
    for node_id in parsed_node_ids:
        node = get_node_by_id(node_id, timeout_seconds=300)
        if node is None:
            raise HTTPException(status_code=404, detail=f"Node {node_id} not found.")

        serial = str(node.get("serial") or "").strip()
        if not serial:
            raise HTTPException(status_code=400, detail=f"Node {node_id} has no serial number.")

        resolved_nodes.append(
            {
                "node_id": node_id,
                "serial": serial,
                "label": node.get("label") or f"Node {node_id}",
            }
        )

    export_dir = create_temp_export_dir(prefix="sensor_export_")
    zip_path = create_temp_export_path(prefix="sensor_export_", suffix=".zip")

    matched_source_files: list[Path] = []
    node_file_counts: dict[str, int] = {}
    exported_files: dict[str, list[str]] = {}

    try:
        for node in resolved_nodes:
            serial = str(node["serial"])

            matching_files = find_sensor_files_for_serial(
                serial=serial,
                start_iso=start_iso,
                end_exclusive_iso=end_exclusive_iso,
            )

            node_file_counts[serial] = len(matching_files)
            exported_files[serial] = [path.name for path in matching_files]
            matched_source_files.extend(matching_files)

        if not matched_source_files:
            raise HTTPException(
                status_code=404,
                detail="No sensor files matched the selected nodes and day range.",
            )

        # Copy source files into the temp export folder first so the outbound ZIP
        # contains a stable snapshot of the matched backend files.
        staged_file_paths = stage_files_for_zip(export_dir, matched_source_files)

        metadata_path = export_dir / "export_metadata.txt"
        metadata_text = build_raw_sensor_export_metadata_text(
            generated_at=utc_now_iso(),
            start_day=start_day,
            end_day=end_day,
            nodes=resolved_nodes,
            node_file_counts=node_file_counts,
            exported_files=exported_files,
        )
        write_text_file(metadata_path, metadata_text)

        zip_members = [metadata_path, *staged_file_paths]
        create_zip_from_paths(zip_path=zip_path, members=zip_members)

    except HTTPException:
        remove_temp_file(zip_path)
        remove_temp_dir(export_dir)
        raise
    except Exception as exc:
        remove_temp_file(zip_path)
        remove_temp_dir(export_dir)
        raise HTTPException(
            status_code=500,
            detail=f"Failed to export sensor data: {exc}",
        ) from exc

    filename = build_sensor_export_zip_filename(
        start_day=start_day,
        end_day=end_day,
        node_count=len(resolved_nodes),
    )

    return FileResponse(
        path=str(zip_path),
        media_type="application/zip",
        filename=filename,
        background=BackgroundTask(_cleanup_sensor_export, export_dir, zip_path),
    )