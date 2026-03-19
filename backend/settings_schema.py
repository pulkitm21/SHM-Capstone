from pydantic import BaseModel, Field
from typing import Dict, Optional, Literal


class SensorMetaModel(BaseModel):
    model: str = ""
    serial: str = ""
    installationDate: str = ""
    location: str = ""
    orientation: str = ""


# Accelerometer config must match the real node configure payload.
class AccelerometerConfigModel(BaseModel):
    odr_index: Literal[0, 1, 2] = 2
    range: Literal[1, 2, 3] = 1
    hpf_corner: int = Field(0, ge=0, le=6)

    # Desired values are what the user most recently requested.
    desired_odr_index: Literal[0, 1, 2] = 2
    desired_range: Literal[1, 2, 3] = 1
    desired_hpf_corner: int = Field(0, ge=0, le=6)

    # Applied values come from the node ACK later.
    applied_odr_index: Optional[Literal[0, 1, 2]] = None
    applied_range: Optional[Literal[1, 2, 3]] = None
    applied_hpf_corner: Optional[int] = Field(default=None, ge=0, le=6)

    # Runtime state indicators shown in the UI.
    current_state: str = "unknown"
    pending_seq: Optional[int] = None
    applied_seq: Optional[int] = None
    last_ack_at: Optional[str] = None
    sync_status: str = "unknown"  # unknown | pending | synced | failed


# Keep a lighter generic model for the non-configurable sensors for now.
class SensorConfigModel(BaseModel):
    samplingRate: str = "200"
    measurementRange: str = "2g"
    lowPassFilter: str = "none"


class SettingsModel(BaseModel):
    site_name: str = "Cape Scott, BC"
    meta: Dict[str, Dict[str, SensorMetaModel]] = Field(default_factory=dict)
    config: Dict[str, Dict[str, dict]] = Field(default_factory=dict)


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
        "accelerometer": to_dict(AccelerometerConfigModel()),
        "inclinometer": to_dict(
            SensorConfigModel(
                samplingRate="20",
                measurementRange="fixed",
                lowPassFilter="fixed",
            )
        ),
        "temperature": to_dict(
            SensorConfigModel(
                samplingRate="1",
                measurementRange="fixed",
                lowPassFilter="fixed",
            )
        ),
    }


DEFAULT_SETTINGS = SettingsModel(
    site_name="Cape Scott, BC",
    meta={},
    config={},
)