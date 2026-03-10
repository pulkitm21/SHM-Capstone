from fastapi import FastAPI, Query, Request
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import StreamingResponse
from datetime import datetime, timezone
from typing import List, Dict, Any, Optional
import asyncio
import json
import struct
import sqlite3
import shutil
from pathlib import Path

from settings_schema import SettingsModel, to_dict, copy_deep
from settings_store import load_settings, save_settings, ensure_node_defaults
from node_registry import list_nodes, get_node_by_id

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
DATA_DIR.mkdir(parents=True, exist_ok=True)

ACCEL_BIN = DATA_DIR / "accel_data_20260219.bin"
INCL_BIN = DATA_DIR / "incl_data.bin"
TEMP_BIN = DATA_DIR / "temp_data.bin"

FAULT_DIR = Path("/mnt/ssd/fault")
FAULT_DIR.mkdir(parents=True, exist_ok=True)
FAULTS_DB = FAULT_DIR / "faults.db"

ACCEL_FORMAT = "<dfff"
ACCEL_SIZE = struct.calcsize(ACCEL_FORMAT)

INCL_FORMAT = "<dff"
INCL_SIZE = struct.calcsize(INCL_FORMAT)

TEMP_FORMAT = "<df"
TEMP_SIZE = struct.calcsize(TEMP_FORMAT)


def _read_tail_bytes(path: Path, record_size: int, max_records: int) -> bytes:
    if not path.exists():
        return b""
    size = path.stat().st_size
    bytes_needed = min(size, max_records * record_size)
    if bytes_needed <= 0:
        return b""
    with path.open("rb") as f:
        f.seek(size - bytes_needed)
        return f.read()


def _iso_from_epoch_seconds(ts: float) -> str:
    return datetime.fromtimestamp(ts, tz=timezone.utc).isoformat()


def read_accel_points(minutes: int, channel: str, max_records_to_scan: int = 50000, limit: int = 500):
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

    for (ts, ax, ay, az) in struct.iter_unpack(ACCEL_FORMAT, data):
        if ts < cutoff_s:
            continue
        val = (ts, ax, ay, az)[idx]
        points.append({"ts": _iso_from_epoch_seconds(ts), "value": float(val)})

    return points[-limit:]


def read_inclinometer_points(minutes: int, channel: str, max_records_to_scan: int = 50000, limit: int = 500):
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

    for (ts, pitch, roll) in struct.iter_unpack(INCL_FORMAT, data):
        if ts < cutoff_s:
            continue
        val = (ts, pitch, roll)[idx]
        points.append({"ts": _iso_from_epoch_seconds(ts), "value": float(val)})

    return points[-limit:]


def read_temperature_points(minutes: int, max_records_to_scan: int = 50000, limit: int = 500):
    now_s = datetime.now(timezone.utc).timestamp()
    cutoff_s = now_s - (minutes * 60)

    data = _read_tail_bytes(TEMP_BIN, TEMP_SIZE, max_records_to_scan)
    if not data:
        return []

    usable_len = (len(data) // TEMP_SIZE) * TEMP_SIZE
    data = data[:usable_len]

    points: List[Dict[str, Any]] = []
    for (ts, temp) in struct.iter_unpack(TEMP_FORMAT, data):
        if ts < cutoff_s:
            continue
        points.append({"ts": _iso_from_epoch_seconds(ts), "value": float(temp)})

    return points[-limit:]


@app.get("/")
def root():
    return {"message": "backend working"}


@app.get("/health")
def health():
    # Testing/manual health check endpoint only. SSE is used for backend status updates in the dashboard.
    return {
        "status": "OK",
        "time": datetime.now(timezone.utc).isoformat(),
    }


@app.get("/api/events/health")
async def health_events(request: Request):
    # SSE code for backend status live updates on the frontend dashboard.
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
    total, used, free = shutil.disk_usage("/mnt/ssd")
    return {
        "total_gb": round(total / (1024 ** 3), 2),
        "used_gb": round(used / (1024 ** 3), 2),
        "free_gb": round(free / (1024 ** 3), 2),
        "usage_percent": round((used / total) * 100, 2) if total > 0 else 0,
    }


@app.get("/api/nodes")
def get_nodes():
    return {"nodes": list_nodes(timeout_seconds=60)}


@app.get("/api/settings")
def get_settings():
    for node in list_nodes(timeout_seconds=60):
        ensure_node_defaults(node["node_id"])

    settings = load_settings()
    return to_dict(settings)


@app.put("/api/settings")
def put_settings(payload: SettingsModel):
    merged = copy_deep(load_settings())

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


@app.get("/api/faults")
def get_faults(
    serial_number: Optional[str] = Query(default=None),
    limit: int = Query(default=200, ge=1, le=5000),
):
    if not FAULTS_DB.exists():
        return {"faults": []}

    con = sqlite3.connect(str(FAULTS_DB))
    con.row_factory = sqlite3.Row
    cur = con.cursor()

    if serial_number is None:
        cur.execute(
            """
            SELECT id, serial_number, sensor_type, fault_type, severity, fault_status, description, ts
            FROM faults
            ORDER BY ts DESC
            LIMIT ?
            """,
            (limit,),
        )
    else:
        cur.execute(
            """
            SELECT id, serial_number, sensor_type, fault_type, severity, fault_status, description, ts
            FROM faults
            WHERE serial_number = ?
            ORDER BY ts DESC
            LIMIT ?
            """,
            (serial_number, limit),
        )

    rows = [dict(r) for r in cur.fetchall()]
    con.close()

    return {"faults": rows}


@app.get("/api/accel")
def get_accel_data(
    node: int = Query(1, ge=1),
    channel: str = Query("x"),
    minutes: int = Query(60, ge=1, le=1440),
):
    channel = channel.lower()
    pts = read_accel_points(minutes=minutes, channel=channel)
    return {"sensor": "accelerometer", "unit": "g", "node": node, "channel": channel, "points": pts}


@app.get("/api/inclinometer")
def api_inclinometer(
    node: int = Query(1, ge=1),
    channel: str = Query("pitch"),
    minutes: int = Query(60, ge=1, le=1440),
):
    channel = channel.lower()
    pts = read_inclinometer_points(minutes=minutes, channel=channel)
    return {"sensor": "inclinometer", "unit": "deg", "node": node, "channel": channel, "points": pts}


@app.get("/api/temperature")
def api_temperature(
    node: int = Query(1, ge=1),
    minutes: int = Query(60, ge=1, le=1440),
):
    pts = read_temperature_points(minutes=minutes)
    return {"sensor": "temperature", "unit": "C", "node": node, "points": pts}