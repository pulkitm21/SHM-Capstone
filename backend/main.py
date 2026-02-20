from fastapi import FastAPI, Query
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field
from datetime import datetime, timezone
from typing import Optional, List, Dict, Any, Tuple
import json
import struct
from pathlib import Path
 
app = FastAPI()
 
# ----------------------------
# CORS
# ----------------------------
app.add_middleware(
    CORSMiddleware,
    allow_origins=["http://localhost:5173", "http://192.168.20.34:5173"],  # add other origins as needed
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)
 
# ----------------------------
# Paths
# ----------------------------
DATA_DIR = Path("/home/pi/Data")
DATA_DIR.mkdir(parents=True, exist_ok=True)
 
# Switch from CSV -> BIN
ACCEL_BIN = DATA_DIR / "accel_data_20260219.bin"
INCL_BIN  = DATA_DIR / "incl_data.bin"
TEMP_BIN  = DATA_DIR / "temp_data.bin"
 
SETTINGS_JSON = DATA_DIR / "settings.json"
 
# ----------------------------
# Pydantic v1/v2 compatibility helpers
# (Keeps this file working whether your Pi has Pydantic v1 or v2)
# ----------------------------
def to_dict(model: BaseModel) -> dict:
    if hasattr(model, "model_dump"):  # Pydantic v2
        return model.model_dump()
    return model.dict()              # Pydantic v1
 
def validate_model(model_cls, data: dict):
    if hasattr(model_cls, "model_validate"):  # Pydantic v2
        return model_cls.model_validate(data)
    return model_cls.parse_obj(data)          # Pydantic v1
 
def copy_deep(model: BaseModel):
    if hasattr(model, "model_copy"):  # Pydantic v2
        return model.model_copy(deep=True)
    return model.copy(deep=True)      # Pydantic v1
 
# ----------------------------
# Models for settings
# ----------------------------
class SensorMetaModel(BaseModel):
    model: str = ""
    serial: str = ""
    installationDate: str = ""   # "YYYY-MM-DD"
    location: str = ""
    orientation: str = ""
 
class SensorConfigModel(BaseModel):
    samplingRate: str = "400"        # "100" | "200" | "400"
    measurementRange: str = "2g"     # "2g" | "4g" | "8g"
    lowPassFilter: str = "100"       # "none" | "50" | "100"
    highPassFilter: str = "none"     # "none" | "1" | "5"
 
class SettingsModel(BaseModel):
    meta: Dict[str, SensorMetaModel] = Field(default_factory=dict)
    config: Dict[str, SensorConfigModel] = Field(default_factory=dict)
 
DEFAULT_SETTINGS = SettingsModel(
    meta={
        "accelerometer": SensorMetaModel(
            model="ADXL355",
            serial="SN00023",
            installationDate="2024-03-15",
            location="Tower",
            orientation="+X +Y +Z",
        ),
        "inclinometer": SensorMetaModel(
            model="SCL3300",
            serial="SN00110",
            installationDate="2024-03-15",
            location="Foundation",
            orientation="Pitch/Roll",
        ),
        "temperature": SensorMetaModel(
            model="ADT7420",
            serial="SN00402",
            installationDate="2024-03-15",
            location="Tower",
            orientation="N/A",
        ),
    },
    config={
        "accelerometer": SensorConfigModel(
            samplingRate="400",
            measurementRange="2g",
            lowPassFilter="100",
            highPassFilter="none",
        ),
        "inclinometer": SensorConfigModel(
            samplingRate="200",
            measurementRange="2g",
            lowPassFilter="50",
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
 
def save_settings(settings: SettingsModel) -> None:
    SETTINGS_JSON.write_text(json.dumps(to_dict(settings), indent=2), encoding="utf-8")
 
def load_settings() -> SettingsModel:
    if not SETTINGS_JSON.exists():
        save_settings(DEFAULT_SETTINGS)
        return DEFAULT_SETTINGS
 
    try:
        raw = json.loads(SETTINGS_JSON.read_text(encoding="utf-8"))
        parsed = validate_model(SettingsModel, raw)
 
        merged = copy_deep(DEFAULT_SETTINGS)
        merged.meta.update(parsed.meta)
        merged.config.update(parsed.config)
        return merged
    except Exception:
        save_settings(DEFAULT_SETTINGS)
        return DEFAULT_SETTINGS
 
# ----------------------------
# Binary formats (struct)
# IMPORTANT: use explicit endianness to avoid platform alignment issues
# Your accel writer uses: PACKET_FORMAT = "dfff"
# We'll read as little-endian standard sizes: "<dfff"
# ----------------------------
ACCEL_FORMAT = "<dfff"  # timestamp(float64 seconds), ax(float32), ay(float32), az(float32)
ACCEL_SIZE = struct.calcsize(ACCEL_FORMAT)
 
# Assumptions for other sensors (adjust if your writer differs):
INCL_FORMAT = "<dff"    # timestamp(float64), pitch(float32), roll(float32)
INCL_SIZE = struct.calcsize(INCL_FORMAT)
 
TEMP_FORMAT = "<df"     # timestamp(float64), temp(float32)
TEMP_SIZE = struct.calcsize(TEMP_FORMAT)
 
def _read_tail_bytes(path: Path, record_size: int, max_records: int) -> bytes:
    """
    Reads only the last max_records records (fast + RAM-friendly).
    """
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
    # Your timestamp is stored as float seconds (epoch)
    return datetime.fromtimestamp(ts, tz=timezone.utc).isoformat()
 
def read_accel_points(minutes: int, channel: str, max_records_to_scan: int = 50000, limit: int = 500):
    """
    channel: x|y|z|all
    Returns points compatible with your existing graph format.
    """
    now_s = datetime.now(timezone.utc).timestamp()
    cutoff_s = now_s - (minutes * 60)
 
    data = _read_tail_bytes(ACCEL_BIN, ACCEL_SIZE, max_records_to_scan)
    if not data:
        return []
 
    usable_len = (len(data) // ACCEL_SIZE) * ACCEL_SIZE
    data = data[:usable_len]
 
    points: List[Dict[str, Any]] = []
 
    if channel == "all":
        # return ax/ay/az in each point (useful if frontend wants all)
        for (ts, ax, ay, az) in struct.iter_unpack(ACCEL_FORMAT, data):
            if ts < cutoff_s:
                continue
            points.append({"ts": _iso_from_epoch_seconds(ts), "ax": float(ax), "ay": float(ay), "az": float(az)})
        return points[-limit:]
 
    idx = {"x": 1, "y": 2, "z": 3}.get(channel, 1)  # default to x
    for (ts, ax, ay, az) in struct.iter_unpack(ACCEL_FORMAT, data):
        if ts < cutoff_s:
            continue
        val = (ts, ax, ay, az)[idx]
        points.append({"ts": _iso_from_epoch_seconds(ts), "value": float(val)})
 
    return points[-limit:]
 
def read_inclinometer_points(minutes: int, channel: str, max_records_to_scan: int = 50000, limit: int = 500):
    """
    channel: pitch|roll
    """
    now_s = datetime.now(timezone.utc).timestamp()
    cutoff_s = now_s - (minutes * 60)
 
    data = _read_tail_bytes(INCL_BIN, INCL_SIZE, max_records_to_scan)
    if not data:
        return []
 
    usable_len = (len(data) // INCL_SIZE) * INCL_SIZE
    data = data[:usable_len]
 
    idx = 1 if channel != "roll" else 2  # (ts, pitch, roll)
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
 
# ----------------------------
# Basic endpoints
# ----------------------------
@app.get("/")
def root():
    return {"message": "backend working"}
 
@app.get("/health")
def health():
    return {"status": "OK"}
 
# ----------------------------
# Settings endpoints
# ----------------------------
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
 
# ----------------------------
# Sensor endpoints (BINARY)
# ----------------------------
@app.get("/api/accel")
def get_accel_data(
    channel: str = Query("x"),  # x | y | z | all
    minutes: int = Query(60, ge=1, le=1440),
):
    channel = channel.lower()
    pts = read_accel_points(minutes=minutes, channel=channel)
    # If channel=all, points contain ax/ay/az instead of "value"
    return {"sensor": "accelerometer", "unit": "g", "channel": channel, "points": pts}
 
@app.get("/api/inclinometer")
def api_inclinometer(
    channel: str = Query("pitch"),  # pitch | roll
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