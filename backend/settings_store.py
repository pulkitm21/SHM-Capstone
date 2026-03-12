import json
from pathlib import Path
from uuid import uuid4

from settings_schema import (
    DEFAULT_SETTINGS,
    SettingsModel,
    SensorConfigModel,
    build_default_node_meta,
    build_default_node_config,
    validate_model,
    copy_deep,
    to_dict,
)


SETTINGS_JSON = Path("/mnt/ssd/settings.json")


def save_settings(settings) -> None:
    SETTINGS_JSON.write_text(json.dumps(to_dict(settings), indent=2), encoding="utf-8")


def load_settings():
    if not SETTINGS_JSON.exists():
        save_settings(DEFAULT_SETTINGS)
        return copy_deep(DEFAULT_SETTINGS)

    try:
        raw = json.loads(SETTINGS_JSON.read_text(encoding="utf-8"))
        parsed = validate_model(SettingsModel, raw)

        merged = copy_deep(DEFAULT_SETTINGS)
        merged.meta.update(parsed.meta)
        merged.config.update(parsed.config)
        return merged
    except Exception:
        save_settings(DEFAULT_SETTINGS)
        return copy_deep(DEFAULT_SETTINGS)


def ensure_node_defaults(node_id: int):
    settings = load_settings()
    key = str(node_id)
    changed = False

    if key not in settings.meta:
        settings.meta[key] = build_default_node_meta()
        changed = True

    if key not in settings.config:
        settings.config[key] = build_default_node_config()
        changed = True

    if changed:
        save_settings(settings)

    return settings


def update_accelerometer_hpf_request(node_id: int, desired: str):
    """
    Update accelerometer HPF request state.

    This is used when the frontend requests an HPF change.
    It updates the desired value and marks the config as pending until
    the ESP32 confirms the applied state later via MQTT ACK.
    """
    settings = load_settings()
    key = str(node_id)

    if key not in settings.config:
        settings.config[key] = {}

    current_accel = settings.config[key].get("accelerometer")
    if current_accel is None:
        current_accel = SensorConfigModel()

    request_id = str(uuid4())

    updated_accel = SensorConfigModel(
        samplingRate=current_accel.samplingRate,
        measurementRange=current_accel.measurementRange,
        lowPassFilter=current_accel.lowPassFilter,
        highPassFilterDesired=desired,
        highPassFilterApplied=current_accel.highPassFilterApplied,
        highPassFilterStatus="pending",
        lastRequestId=request_id,
        lastAckAt=None,
    )

    settings.config[key]["accelerometer"] = updated_accel
    save_settings(settings)

    return updated_accel