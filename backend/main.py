from fastapi import FastAPI, Query, Request, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import StreamingResponse
from datetime import datetime, timezone
from typing import List, Dict, Any, Optional
from pydantic import BaseModel, Field
import asyncio
import json
import struct
import sqlite3
import shutil
import os
from pathlib import Path

from settings_schema import SettingsModel, to_dict, copy_deep
from settings_store import (
    load_settings,
    save_settings,
    ensure_node_defaults,
    update_accelerometer_config_request,
    get_site_name,
    update_site_name,
)
from node_registry import list_nodes, get_node_by_id, update_node_position

app = FastAPI()

app.add_middleware(
    CORSMiddleware,
    allow_origins=[
        "http://localhost:5173",
        "http://192.168.20.34:5173",
    ],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

DATA_DIR = Path("/mnt/ssd")

ACCEL_BIN = DATA_DIR / "accel_data_20260219.bin"
INCL_BIN = DATA_DIR / "incl_data.bin"
TEMP_BIN = DATA_DIR / "temp_data.bin"
FAULT_DIR = DATA_DIR / "fault"
FAULTS_DB = FAULT_DIR / "faults.db"

ACCEL_FORMAT = "<dfff"
ACCEL_SIZE = struct.calcsize(ACCEL_FORMAT)

INCL_FORMAT = "<dff"
INCL_SIZE = struct.calcsize(INCL_FORMAT)

TEMP_FORMAT = "<df"
TEMP_SIZE = struct.calcsize(TEMP_FORMAT)


class NodePositionUpdate(BaseModel):
    x: float = Field(..., ge=0.0, le=1.0)
    y: float = Field(..., ge=0.0, le=1.0)


class AccelerometerConfigApplyRequest(BaseModel):
    odr_index: int = Field(..., ge=0, le=2)
    range: int = Field(..., ge=1, le=3)
    hpf_corner: int = Field(..., ge=0, le=6)


class NodeControlRequest(BaseModel):
    cmd: str = Field(..., pattern="^(start|stop|init|reset)$")


class SiteNameUpdate(BaseModel):
    site_name: str = Field(..., min_length=1, max_length=60)


def _iso_from_epoch_seconds(ts: float) -> str:
    return datetime.fromtimestamp(ts, tz=timezone.utc).isoformat()


def get_ssd_mount_status() -> Dict[str, Any]:
    """
    Diagnostic check for SSD mount health.
    Verifies whether /mnt/ssd exists, is mounted, and is accessible.
    """
    mount_path = DATA_DIR

    exists = mount_path.exists()
    mounted = os.path.ismount(mount_path)
    readable = os.access(mount_path, os.R_OK) if exists else False
    writable = os.access(mount_path, os.W_OK) if exists else False

    available = bool(exists and mounted and readable)

    return {
        "mount_path": str(mount_path),
        "exists": exists,
        "mounted": mounted,
        "readable": readable,
        "writable": writable,
        "available": available,
        "status": "mounted" if available else "unavailable",
        "time": datetime.now(timezone.utc).isoformat(),
    }


def is_ssd_available() -> bool:
    return bool(get_ssd_mount_status()["available"])


def empty_fault_response(page: int = 1, page_size: int = 15) -> Dict[str, Any]:
    safe_page = max(1, page)
    safe_page_size = max(1, page_size)

    return {
        "faults": [],
        "page": safe_page,
        "page_size": safe_page_size,
        "total_items": 0,
        "total_pages": 1,
        "filter_options": {
            "sensor_types": [],
            "fault_types": [],
            "severities": [],
            "statuses": [],
        },
    }


def _read_tail_bytes(path: Path, record_size: int, max_records: int) -> bytes:
    """
    Safely read the trailing bytes of a binary sensor file.

    If the SSD is missing or the file cannot be read, return empty bytes so the
    caller can degrade gracefully instead of failing the request.
    """
    if not is_ssd_available():
        return b""

    try:
        if not path.exists():
            return b""

        size = path.stat().st_size
        bytes_needed = min(size, max_records * record_size)

        if bytes_needed <= 0:
            return b""

        with path.open("rb") as f:
            f.seek(size - bytes_needed)
            return f.read()

    except (OSError, ValueError):
        return b""


def read_accel_points(
    minutes: int,
    channel: str,
    max_records_to_scan: int = 50000,
    limit: int = 500,
):
    if not is_ssd_available():
        return []

    now_s = datetime.now(timezone.utc).timestamp()
    cutoff_s = now_s - (minutes * 60)

    if channel not in ["x", "y", "z"]:
        return []

    data = _read_tail_bytes(ACCEL_BIN, ACCEL_SIZE, max_records_to_scan)
    if not data:
        return []

    usable_len = (len(data) // ACCEL_SIZE) * ACCEL_SIZE
    data = data[:usable_len]

    idx = {"x": 1, "y": 2, "z": 3}[channel]
    points: List[Dict[str, Any]] = []

    try:
        for (ts, ax, ay, az) in struct.iter_unpack(ACCEL_FORMAT, data):
            if ts < cutoff_s:
                continue

            val = (ts, ax, ay, az)[idx]
            points.append({"ts": _iso_from_epoch_seconds(ts), "value": float(val)})

    except struct.error:
        return []

    return points[-limit:]


def read_inclinometer_points(
    minutes: int,
    channel: str,
    max_records_to_scan: int = 50000,
    limit: int = 500,
):
    if not is_ssd_available():
        return []

    now_s = datetime.now(timezone.utc).timestamp()
    cutoff_s = now_s - (minutes * 60)

    if channel not in ["pitch", "roll"]:
        return []

    data = _read_tail_bytes(INCL_BIN, INCL_SIZE, max_records_to_scan)
    if not data:
        return []

    usable_len = (len(data) // INCL_SIZE) * INCL_SIZE
    data = data[:usable_len]

    idx = 1 if channel == "pitch" else 2
    points: List[Dict[str, Any]] = []

    try:
        for (ts, pitch, roll) in struct.iter_unpack(INCL_FORMAT, data):
            if ts < cutoff_s:
                continue

            val = (ts, pitch, roll)[idx]
            points.append({"ts": _iso_from_epoch_seconds(ts), "value": float(val)})

    except struct.error:
        return []

    return points[-limit:]


def read_temperature_points(
    minutes: int,
    max_records_to_scan: int = 50000,
    limit: int = 500,
):
    if not is_ssd_available():
        return []

    now_s = datetime.now(timezone.utc).timestamp()
    cutoff_s = now_s - (minutes * 60)

    data = _read_tail_bytes(TEMP_BIN, TEMP_SIZE, max_records_to_scan)
    if not data:
        return []

    usable_len = (len(data) // TEMP_SIZE) * TEMP_SIZE
    data = data[:usable_len]

    points: List[Dict[str, Any]] = []

    try:
        for (ts, temp) in struct.iter_unpack(TEMP_FORMAT, data):
            if ts < cutoff_s:
                continue

            points.append({"ts": _iso_from_epoch_seconds(ts), "value": float(temp)})

    except struct.error:
        return []

    return points[-limit:]


@app.get("/")
def root():
    return {"message": "backend working"}


@app.get("/health")
def health():
    # Testing/manual health check endpoint only.
    # SSE is used for backend status updates in the dashboard.
    return {
        "status": "OK",
        "time": datetime.now(timezone.utc).isoformat(),
    }


@app.get("/api/events/health")
async def health_events(request: Request):
    # Backend liveness SSE. This should stay independent of SSD state.
    async def event_generator():
        while True:
            if await request.is_disconnected():
                break

            payload = {
                "status": "OK",
                "time": datetime.now(timezone.utc).isoformat(),
            }

            yield f"data: {json.dumps(payload)}\n\n"
            await asyncio.sleep(5)

    return StreamingResponse(
        event_generator(),
        media_type="text/event-stream",
        headers={
            "Cache-Control": "no-cache",
            "Connection": "keep-alive",
        },
    )


@app.get("/api/storage")
def get_storage():
    mount_status = get_ssd_mount_status()

    if not mount_status["available"]:
        return {
            "total_gb": 0,
            "used_gb": 0,
            "free_gb": 0,
            "usage_percent": 0,
            "ssd_status": mount_status,
        }

    try:
        total, used, free = shutil.disk_usage(DATA_DIR)
        return {
            "total_gb": round(total / (1024 ** 3), 2),
            "used_gb": round(used / (1024 ** 3), 2),
            "free_gb": round(free / (1024 ** 3), 2),
            "usage_percent": round((used / total) * 100, 2) if total > 0 else 0,
            "ssd_status": mount_status,
        }
    except OSError:
        return {
            "total_gb": 0,
            "used_gb": 0,
            "free_gb": 0,
            "usage_percent": 0,
            "ssd_status": {
                **mount_status,
                "available": False,
                "status": "unavailable",
            },
        }


@app.get("/api/storage/status")
def get_storage_status():
    """
    Returns SSD mount diagnostic information for the dashboard.
    """
    return get_ssd_mount_status()


@app.get("/api/nodes")
def get_nodes():
    return {"nodes": list_nodes(timeout_seconds=60)}


@app.get("/api/nodes/{node_id}")
def get_node(node_id: int):
    node = get_node_by_id(node_id, timeout_seconds=60)
    if node is None:
        raise HTTPException(status_code=404, detail="Node not found")
    return {"node": node}


@app.put("/api/nodes/{node_id}/position")
def put_node_position(node_id: int, payload: NodePositionUpdate):
    updated = update_node_position(node_id=node_id, x=payload.x, y=payload.y)
    if updated is None:
        raise HTTPException(status_code=404, detail="Node not found")
    return {"ok": True, "node": updated}


# =========================
# Bulk node position update endpoint
# Allows frontend to save all node positions in a single request
# Used for "Edit → Move → Save" workflow in NodeMap
# =========================
class BulkNodePositionItem(BaseModel):
    node_id: int
    x: float = Field(..., ge=0.0, le=1.0)
    y: float = Field(..., ge=0.0, le=1.0)


class BulkNodePositionUpdate(BaseModel):
    positions: List[BulkNodePositionItem]


@app.put("/api/nodes/positions")
def put_node_positions(payload: BulkNodePositionUpdate):
    updated_nodes = []

    for item in payload.positions:
        updated = update_node_position(
            node_id=item.node_id,
            x=item.x,
            y=item.y,
        )

        if updated is None:
            raise HTTPException(
                status_code=404,
                detail=f"Node {item.node_id} not found",
            )

        updated_nodes.append(updated)

    return {
        "ok": True,
        "nodes": updated_nodes,
    }


@app.get("/api/settings")
def get_settings():
    for node in list_nodes(timeout_seconds=60):
        ensure_node_defaults(node["node_id"])

    settings = load_settings()
    return to_dict(settings)


@app.put("/api/settings")
def put_settings(payload: SettingsModel):
    merged = copy_deep(load_settings())

    merged.site_name = payload.site_name

    for node_id, per_sensor_meta in payload.meta.items():
        if node_id not in merged.meta:
            merged.meta[node_id] = {}
        merged.meta[node_id].update(per_sensor_meta)

    for node_id, per_sensor_cfg in payload.config.items():
        if node_id not in merged.config:
            merged.config[node_id] = {}
        merged.config[node_id].update(per_sensor_cfg)

    save_settings(merged)
    return {"ok": True, "settings": to_dict(merged)}


@app.get("/api/settings/site-name")
def api_get_site_name():
    return {"site_name": get_site_name()}


@app.put("/api/settings/site-name")
def api_put_site_name(payload: SiteNameUpdate):
    updated_name = update_site_name(payload.site_name)
    return {"ok": True, "site_name": updated_name}


@app.post("/api/nodes/{node_id}/config/accelerometer/apply")
def apply_accelerometer_config(node_id: int, payload: AccelerometerConfigApplyRequest):
    node = get_node_by_id(node_id, timeout_seconds=60)
    if node is None:
        raise HTTPException(status_code=404, detail="Node not found")

    ensure_node_defaults(node_id)

    seq = int(datetime.now(timezone.utc).timestamp() * 1000)

    updated = update_accelerometer_config_request(
        node_id=node_id,
        odr_index=payload.odr_index,
        range_value=payload.range,
        hpf_corner=payload.hpf_corner,
        seq=seq,
    )

    # Phase 1 only: store the request and return it.
    # MQTT publish will be added in Phase 2.
    return {
        "ok": True,
        "node_id": node["node_id"],
        "serial": node["serial"],
        "sensor": "accelerometer",
        "desired": {
            "odr_index": updated.desired_odr_index,
            "range": updated.desired_range,
            "hpf_corner": updated.desired_hpf_corner,
        },
        "applied": {
            "odr_index": updated.applied_odr_index,
            "range": updated.applied_range,
            "hpf_corner": updated.applied_hpf_corner,
        },
        "current_state": updated.current_state,
        "pending_seq": updated.pending_seq,
        "applied_seq": updated.applied_seq,
        "sync_status": updated.sync_status,
        "acked_at": updated.last_ack_at,
    }


@app.post("/api/nodes/{node_id}/control")
def control_node(node_id: int, payload: NodeControlRequest):
    node = get_node_by_id(node_id, timeout_seconds=60)
    if node is None:
        raise HTTPException(status_code=404, detail="Node not found")

    # Phase 1 only: backend accepts the request shape.
    # MQTT control publish will be added in Phase 2.
    return {
        "ok": True,
        "node_id": node["node_id"],
        "serial": node["serial"],
        "cmd": payload.cmd,
        "status": "accepted",
    }


def _normalize_fault_text(value: Optional[str]) -> str:
    return str(value or "").strip()


def _build_fault_where_clauses(
    serial_number: Optional[str] = None,
    sensor_type: Optional[str] = None,
    fault_type: Optional[str] = None,
    severity: Optional[int] = None,
    fault_status: Optional[str] = None,
    description: Optional[str] = None,
) -> tuple[list[str], list[Any]]:
    where_clauses: list[str] = []
    params: list[Any] = []

    serial_number_value = _normalize_fault_text(serial_number)
    sensor_type_value = _normalize_fault_text(sensor_type)
    fault_type_value = _normalize_fault_text(fault_type)
    fault_status_value = _normalize_fault_text(fault_status)
    description_value = _normalize_fault_text(description)

    if serial_number_value:
        where_clauses.append("serial_number LIKE ?")
        params.append(f"%{serial_number_value}%")

    if sensor_type_value:
        where_clauses.append("LOWER(sensor_type) = LOWER(?)")
        params.append(sensor_type_value)

    if fault_type_value:
        where_clauses.append("LOWER(fault_type) = LOWER(?)")
        params.append(fault_type_value)

    if severity is not None:
        where_clauses.append("severity = ?")
        params.append(severity)

    if fault_status_value:
        where_clauses.append("LOWER(fault_status) = LOWER(?)")
        params.append(fault_status_value)

    if description_value:
        where_clauses.append("description LIKE ?")
        params.append(f"%{description_value}%")

    return where_clauses, params


def _build_fault_where_sql(where_clauses: list[str]) -> str:
    if not where_clauses:
        return ""
    return "WHERE " + " AND ".join(where_clauses)


def _get_fault_filter_options(
    con: sqlite3.Connection,
    serial_number: Optional[str] = None,
) -> Dict[str, List[Any]]:
    serial_number_value = _normalize_fault_text(serial_number)

    if serial_number_value:
        serial_number_where_sql = "WHERE serial_number LIKE ?"
        serial_number_params: list[Any] = [f"%{serial_number_value}%"]
    else:
        serial_number_where_sql = ""
        serial_number_params = []

    cur = con.cursor()

    cur.execute(
        f"""
        SELECT DISTINCT sensor_type
        FROM faults
        {serial_number_where_sql}
        ORDER BY sensor_type COLLATE NOCASE ASC
        """,
        serial_number_params,
    )
    sensor_types = [row[0] for row in cur.fetchall() if row[0] not in (None, "")]

    cur.execute(
        f"""
        SELECT DISTINCT fault_type
        FROM faults
        {serial_number_where_sql}
        ORDER BY fault_type COLLATE NOCASE ASC
        """,
        serial_number_params,
    )
    fault_types = [row[0] for row in cur.fetchall() if row[0] not in (None, "")]

    cur.execute(
        f"""
        SELECT DISTINCT severity
        FROM faults
        {serial_number_where_sql}
        ORDER BY severity ASC
        """,
        serial_number_params,
    )
    severities = [row[0] for row in cur.fetchall() if row[0] is not None]

    cur.execute(
        f"""
        SELECT DISTINCT fault_status
        FROM faults
        {serial_number_where_sql}
        ORDER BY fault_status COLLATE NOCASE ASC
        """,
        serial_number_params,
    )
    statuses = [row[0] for row in cur.fetchall() if row[0] not in (None, "")]

    return {
        "sensor_types": sensor_types,
        "fault_types": fault_types,
        "severities": severities,
        "statuses": statuses,
    }


def read_fault_rows(
    serial_number: Optional[str],
    limit: int,
    page: int = 1,
    page_size: Optional[int] = None,
    sensor_type: Optional[str] = None,
    fault_type: Optional[str] = None,
    severity: Optional[int] = None,
    fault_status: Optional[str] = None,
    description: Optional[str] = None,
) -> Dict[str, Any]:
    effective_page_size = page_size if page_size is not None else limit

    if not is_ssd_available():
        return empty_fault_response(page=page, page_size=effective_page_size)

    if not FAULTS_DB.exists():
        return empty_fault_response(page=page, page_size=effective_page_size)

    safe_page = max(1, page)
    safe_page_size = max(1, effective_page_size)
    offset = (safe_page - 1) * safe_page_size

    con: Optional[sqlite3.Connection] = None

    try:
        con = sqlite3.connect(str(FAULTS_DB))
        con.row_factory = sqlite3.Row
        cur = con.cursor()

        where_clauses, where_params = _build_fault_where_clauses(
            serial_number=serial_number,
            sensor_type=sensor_type,
            fault_type=fault_type,
            severity=severity,
            fault_status=fault_status,
            description=description,
        )
        where_sql = _build_fault_where_sql(where_clauses)

        cur.execute(
            f"""
            SELECT COUNT(*) AS count
            FROM faults
            {where_sql}
            """,
            where_params,
        )
        total_items = int(cur.fetchone()["count"])
        total_pages = max(1, (total_items + safe_page_size - 1) // safe_page_size)

        cur.execute(
            f"""
            SELECT id, serial_number, sensor_type, fault_type, severity, fault_status, description, ts
            FROM faults
            {where_sql}
            ORDER BY ts DESC
            LIMIT ? OFFSET ?
            """,
            [*where_params, safe_page_size, offset],
        )
        rows = [dict(r) for r in cur.fetchall()]

        # Filter dropdowns are read from SQLite so the frontend does not need
        # to scan a large in-memory dataset just to build filter choices.
        filter_options = _get_fault_filter_options(con=con, serial_number=serial_number)

        return {
            "faults": rows,
            "page": safe_page,
            "page_size": safe_page_size,
            "total_items": total_items,
            "total_pages": total_pages,
            "filter_options": filter_options,
        }

    except sqlite3.Error:
        return empty_fault_response(page=safe_page, page_size=safe_page_size)

    finally:
        if con is not None:
            con.close()


@app.get("/api/faults")
def get_faults(
    serial_number: Optional[str] = Query(default=None),
    sensor_type: Optional[str] = Query(default=None),
    fault_type: Optional[str] = Query(default=None),
    severity: Optional[int] = Query(default=None),
    fault_status: Optional[str] = Query(default=None),
    description: Optional[str] = Query(default=None),
    page: int = Query(default=1, ge=1),
    page_size: int = Query(default=15, ge=1, le=200),
    limit: int = Query(default=200, ge=1, le=5000),
):
    return read_fault_rows(
        serial_number=serial_number,
        sensor_type=sensor_type,
        fault_type=fault_type,
        severity=severity,
        fault_status=fault_status,
        description=description,
        page=page,
        page_size=page_size,
        limit=limit,
    )


@app.get("/api/events/faults")
async def fault_events(
    request: Request,
    serial_number: Optional[str] = Query(default=None),
    limit: int = Query(default=200, ge=1, le=5000),
):
    # Fault SSE should also degrade cleanly if SSD or DB is unavailable.
    async def event_generator():
        while True:
            if await request.is_disconnected():
                break

            payload = {
                "faults": read_fault_rows(serial_number=serial_number, limit=limit)["faults"],
                "time": datetime.now(timezone.utc).isoformat(),
            }

            yield f"data: {json.dumps(payload)}\n\n"
            await asyncio.sleep(5)

    return StreamingResponse(
        event_generator(),
        media_type="text/event-stream",
        headers={
            "Cache-Control": "no-cache",
            "Connection": "keep-alive",
        },
    )


@app.get("/api/accel")
def get_accel_data(
    node: int = Query(1, ge=1),
    channel: str = Query("x"),
    minutes: int = Query(60, ge=1, le=1440),
):
    channel = channel.lower()
    pts = read_accel_points(minutes=minutes, channel=channel)
    return {
        "sensor": "accelerometer",
        "unit": "g",
        "node": node,
        "channel": channel,
        "points": pts,
    }


@app.get("/api/inclinometer")
def api_inclinometer(
    node: int = Query(1, ge=1),
    channel: str = Query("pitch"),
    minutes: int = Query(60, ge=1, le=1440),
):
    channel = channel.lower()
    pts = read_inclinometer_points(minutes=minutes, channel=channel)
    return {
        "sensor": "inclinometer",
        "unit": "deg",
        "node": node,
        "channel": channel,
        "points": pts,
    }


@app.get("/api/temperature")
def api_temperature(
    node: int = Query(1, ge=1),
    minutes: int = Query(60, ge=1, le=1440),
):
    pts = read_temperature_points(minutes=minutes)
    return {
        "sensor": "temperature",
        "unit": "C",
        "node": node,
        "points": pts,
    }