from fastapi import FastAPI, Query
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field
from datetime import datetime, timezone
from typing import List, Dict, Any, Optional
import struct
import sqlite3  # added for fault log sqlite access
from pathlib import Path

from settings_store import load_settings, save_settings

app = FastAPI()

# CORS
app.add_middleware(
    CORSMiddleware,
    allow_origins=["http://localhost:5173", "http://192.168.20.34:5173"],  # Allowed IP addresses for frontend access on the local network
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# Paths
DATA_DIR = Path("/home/pi/Data")
DATA_DIR.mkdir(parents=True, exist_ok=True)

ACCEL_BIN = DATA_DIR / "accel_data_20260219.bin"
INCL_BIN  = DATA_DIR / "incl_data.bin"
TEMP_BIN  = DATA_DIR / "temp_data.bin"

# SQLite DB path for fault log
FAULTS_DB = DATA_DIR / "faults.db"

# Pydantic helpers
def to_dict(model: BaseModel) -> dict:
    return model.model_dump() if hasattr(model, "model_dump") else model.dict()

def validate_model(model_cls, data: dict):
    return model_cls.model_validate(data) if hasattr(model_cls, "model_validate") else model_cls.parse_obj(data)

def copy_deep(model: BaseModel):
    return model.model_copy(deep=True) if hasattr(model, "model_copy") else model.copy(deep=True)

# Models
class SensorMetaModel(BaseModel):
    model: str = ""
    serial: str = ""
    installationDate: str = ""
    location: str = ""
    orientation: str = ""

class SensorConfigModel(BaseModel):
    samplingRate: str = "200"
    measurementRange: str = "2g"
    lowPassFilter: str = "none"
    highPassFilter: str = "none"

#  Settings are now node-aware (meta/config keyed by node_id -> sensor)
class SettingsModel(BaseModel):
    meta: Dict[str, Dict[str, SensorMetaModel]] = Field(default_factory=dict)
    config: Dict[str, Dict[str, SensorConfigModel]] = Field(default_factory=dict)

# CHANGE: Node-aware defaults (node "1" included by default)
DEFAULT_SETTINGS = SettingsModel(
    meta={
        "1": {
            "accelerometer": SensorMetaModel(
                model="ADXL355",
                serial="SN00023",
                installationDate="2025-09-15",
                location="Tower",
                orientation="+X +Y +Z",
            ),
            "inclinometer": SensorMetaModel(
                model="SCL3300",
                serial="SN00110",
                installationDate="2025-09-15",
                location="Foundation",
                orientation="+X +Y",
            ),
            "temperature": SensorMetaModel(
                model="ADT7420",
                serial="SN00402",
                installationDate="2025-09-15",
                location="Tower",
                orientation="N/A",
            ),
        }
    },
    config={
        "1": {
            "accelerometer": SensorConfigModel(
                samplingRate="200",
                measurementRange="2g",
                lowPassFilter="none",
                highPassFilter="none",
            ),
            "inclinometer": SensorConfigModel(
                samplingRate="200",
                measurementRange="2g",
                lowPassFilter="none",
                highPassFilter="none",
            ),
            "temperature": SensorConfigModel(
                samplingRate="100",
                measurementRange="2g",
                lowPassFilter="none",
                highPassFilter="none",
            ),
        }
    },
)

# Binary formats
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
    return {"status": "OK"}

@app.get("/api/settings")
def get_settings():
    # CHANGE: ensure node-aware defaults exist even if file is old/missing
    settings = load_settings()
    if not settings.meta or not settings.config:
        settings = copy_deep(DEFAULT_SETTINGS)
        save_settings(settings)
        return to_dict(settings)

    # If settings file exists but is missing node "1", add it
    if "1" not in settings.meta:
        settings.meta["1"] = to_dict(DEFAULT_SETTINGS).get("meta", {}).get("1", {})
    if "1" not in settings.config:
        settings.config["1"] = to_dict(DEFAULT_SETTINGS).get("config", {}).get("1", {})

    save_settings(settings)
    return to_dict(settings)

@app.put("/api/settings")
def put_settings(payload: SettingsModel):
    #  merge node-aware settings safely
    merged = copy_deep(DEFAULT_SETTINGS)

    # Merge meta
    for node_id, per_sensor_meta in payload.meta.items():
        if node_id not in merged.meta:
            merged.meta[node_id] = {}
        merged.meta[node_id].update(per_sensor_meta)

    # Merge config
    for node_id, per_sensor_cfg in payload.config.items():
        if node_id not in merged.config:
            merged.config[node_id] = {}
        merged.config[node_id].update(per_sensor_cfg)

    save_settings(merged)
    return {"ok": True, "settings": to_dict(merged)}

#  /api/faults endpoint for SQLite fault log
@app.get("/api/faults")
def get_faults(
    node: Optional[int] = Query(default=None),
    limit: int = Query(default=200, ge=1, le=5000),
):
    # If DB doesn't exist yet, return empty list
    if not FAULTS_DB.exists():
        return {"faults": []}

    con = sqlite3.connect(str(FAULTS_DB))
    con.row_factory = sqlite3.Row
    cur = con.cursor()

    # match to DB schema.
    # Expected columns: id, node_id, sensor_id, fault_type, severity, ts
    if node is None:
        cur.execute(
            """
            SELECT id, node_id, sensor_id, fault_type, severity, ts
            FROM faults
            ORDER BY ts DESC
            LIMIT ?
            """,
            (limit,),
        )
    else:
        cur.execute(
            """
            SELECT id, node_id, sensor_id, fault_type, severity, ts
            FROM faults
            WHERE node_id = ?
            ORDER BY ts DESC
            LIMIT ?
            """,
            (node, limit),
        )

    rows = [dict(r) for r in cur.fetchall()]
    con.close()
    return {"faults": rows}

@app.get("/api/accel")
def get_accel_data(
    # ---- CHANGE: accept node query param 
    node: int = Query(1, ge=1),
    channel: str = Query("x"),
    minutes: int = Query(60, ge=1, le=1440),
):
    # NOTE: node is accepted for API compatibility. You can later map node -> per-node bin file.
    channel = channel.lower()
    pts = read_accel_points(minutes=minutes, channel=channel)
    return {"sensor": "accelerometer", "unit": "g", "node": node, "channel": channel, "points": pts}

@app.get("/api/inclinometer")
def api_inclinometer(
    # ---- CHANGE: accept node query param 
    node: int = Query(1, ge=1),
    channel: str = Query("pitch"),
    minutes: int = Query(60, ge=1, le=1440),
):
    # NOTE: node is accepted for API compatibility. You can later map node -> per-node bin file.
    channel = channel.lower()
    pts = read_inclinometer_points(minutes=minutes, channel=channel)
    return {"sensor": "inclinometer", "unit": "deg", "node": node, "channel": channel, "points": pts}

@app.get("/api/temperature")
def api_temperature(
    # ---- CHANGE: accept node query param 
    node: int = Query(1, ge=1),
    minutes: int = Query(60, ge=1, le=1440),
):
    # NOTE: node is accepted for API compatibility. You can later map node -> per-node bin file.
    pts = read_temperature_points(minutes=minutes)
    return {"sensor": "temperature", "unit": "C", "node": node, "points": pts}

import shutil
from fastapi import FastAPI

@app.get("/api/storage")
def get_storage():
    
    total, used, free=shutil.disk_usage("/mnt/ssd")
    return {
        "total_gb": round(total/ (1024**3), 2),
        "used_gb": round(used/ (1024**3), 2),
        "free_gb": round(free/ (1024**3), 2),
        "usage_percent": round((used/total) *100, 2)
        }