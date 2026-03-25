from __future__ import annotations

import sqlite3
from typing import Optional

from fastapi import APIRouter, HTTPException, Query
from fastapi.responses import FileResponse
from starlette.background import BackgroundTask

from export_utils import (
    FAULTS_DB,
    build_fault_export_filename,
    build_fault_export_where,
    build_where_sql,
    create_temp_export_path,
    is_fault_db_available,
    remove_temp_file,
    write_fault_csv,
)

router = APIRouter()


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
    """
    Export filtered fault rows to CSV.

    Pi-efficiency notes:
    - SQLite performs filtering.
    - CSV is written to a temp file on disk.
    - Rows are streamed from SQLite in chunks via fetchmany().
    - No giant in-memory CSV string is created.
    """
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