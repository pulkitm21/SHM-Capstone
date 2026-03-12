from pydantic import BaseModel, Field
from typing import Dict, Optional


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

    # desired configuration (what the user requested)
    highPassFilterDesired: str = "none"

    # applied configuration (what the ESP32 last confirmed)
    highPassFilterApplied: Optional[str] = None

    # sync state: unknown | pending | synced | failed
    highPassFilterStatus: str = "unknown"

    # request/ack tracking
    lastRequestId: Optional[str] = None
    lastAckAt: Optional[str] = None


class SettingsModel(BaseModel):
    meta: Dict[str, Dict[str, SensorMetaModel]] = Field(default_factory=dict)
    config: Dict[str, Dict[str, SensorConfigModel]] = Field(default_factory=dict)


def to_dict(model: BaseModel) -> dict:
    return model.model_dump() if hasattr(model, "model_dump") else model.dict()


def validate_model(model_cls, data: dict):
    return model_cls.model_validate(data) if hasattr(model_cls, "model_validate") else model_cls.parse_obj(data)


def copy_deep(model: BaseModel):
    return model.model_copy(deep=True) if hasattr(model, "model_copy") else model.copy(deep=True)


def build_default_node_meta():
    return {
        "accelerometer": SensorMetaModel(
            model="ADXL355",
            serial="ACCEL-001",
            installationDate="2025-09-15",
            location="Tower",
            orientation="+X +Y +Z",
        ),
        "inclinometer": SensorMetaModel(
            model="SCL3300",
            serial="INCL-001",
            installationDate="2025-09-15",
            location="Foundation",
            orientation="+X +Y",
        ),
        "temperature": SensorMetaModel(
            model="ADT7420",
            serial="TEMP-001",
            installationDate="2025-09-15",
            location="Tower",
            orientation="N/A",
        ),
    }


def build_default_node_config():
    return {
        "accelerometer": SensorConfigModel(
            samplingRate="200",
            measurementRange="2g",
            lowPassFilter="none",
            highPassFilterDesired="none",
            highPassFilterApplied=None,
            highPassFilterStatus="unknown",
            lastRequestId=None,
            lastAckAt=None,
        ),
        "inclinometer": SensorConfigModel(
            samplingRate="200",
            measurementRange="2g",
            lowPassFilter="none",
            highPassFilterDesired="none",
            highPassFilterApplied=None,
            highPassFilterStatus="unknown",
            lastRequestId=None,
            lastAckAt=None,
        ),
        "temperature": SensorConfigModel(
            samplingRate="100",
            measurementRange="2g",
            lowPassFilter="none",
            highPassFilterDesired="none",
            highPassFilterApplied=None,
            highPassFilterStatus="unknown",
            lastRequestId=None,
            lastAckAt=None,
        ),
    }


DEFAULT_SETTINGS = SettingsModel(meta={}, config={})