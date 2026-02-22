from fastapi import FastAPI, Query
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field
from datetime import datetime, timezone
from typing import List, Dict, Any
import struct
from pathlib import Path

from settings_store import load_settings, save_settings

app = FastAPI()

# CORS
app.add_middleware(
    CORSMiddleware,
    allow_origins=["http://localhost:5173", "http://192.168.20.34:5173"], # Allowed IP addresses for frontend access on the local network
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

class SettingsModel(BaseModel):
    meta: Dict[str, SensorMetaModel] = Field(default_factory=dict)
    config: Dict[str, SensorConfigModel] = Field(default_factory=dict)

DEFAULT_SETTINGS = SettingsModel(
    meta={
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
    },
    config={
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
    settings = load_settings()
    return to_dict(settings)

@app.put("/api/settings")
def put_settings(payload: SettingsModel):
    merged = copy_deep(DEFAULT_SETTINGS)
    merged.meta.update(payload.meta)
    merged.config.update(payload.config)
    save_settings(merged)
    return {"ok": True, "settings": to_dict(merged)}

@app.get("/api/accel")
def get_accel_data(
    channel: str = Query("x"),
    minutes: int = Query(60, ge=1, le=1440),
):
    channel = channel.lower()
    pts = read_accel_points(minutes=minutes, channel=channel)
    return {"sensor": "accelerometer", "unit": "g", "channel": channel, "points": pts}

@app.get("/api/inclinometer")
def api_inclinometer(
    channel: str = Query("pitch"),
    minutes: int = Query(60, ge=1, le=1440),
):
    channel = channel.lower()
    pts = read_inclinometer_points(minutes=minutes, channel=channel)
    return {"sensor": "inclinometer", "unit": "deg", "channel": channel, "points": pts}

@app.get("/api/temperature")
def api_temperature(
    minutes: int = Query(60, ge=1, le=1440),
):
    pts = read_temperature_points(minutes=minutes)
    return {"sensor": "temperature", "unit": "C", "points": pts}