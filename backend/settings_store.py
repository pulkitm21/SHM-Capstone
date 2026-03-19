import json
from pathlib import Path

from settings_schema import (
    DEFAULT_SETTINGS,
    SettingsModel,
    AccelerometerConfigModel,
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
        merged.site_name = parsed.site_name
        merged.meta.update(parsed.meta)
        merged.config.update(parsed.config)
        return merged
    except Exception:
        save_settings(DEFAULT_SETTINGS)
        return copy_deep(DEFAULT_SETTINGS)


def get_site_name() -> str:
    settings = load_settings()
    return settings.site_name or DEFAULT_SETTINGS.site_name


def update_site_name(site_name: str) -> str:
    settings = load_settings()
    cleaned_name = site_name.strip() or DEFAULT_SETTINGS.site_name
    settings.site_name = cleaned_name
    save_settings(settings)
    return settings.site_name


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


# Store a newly requested accelerometer config and mark it pending.
def update_accelerometer_config_request(
    node_id: int,
    odr_index: int,
    range_value: int,
    hpf_corner: int,
    seq: int,
):
    settings = load_settings()
    key = str(node_id)

    if key not in settings.config:
        settings.config[key] = build_default_node_config()

    current_raw = settings.config[key].get("accelerometer") or {}
    current_accel = validate_model(AccelerometerConfigModel, current_raw)

    updated_accel = AccelerometerConfigModel(
        odr_index=current_accel.odr_index,
        range=current_accel.range,
        hpf_corner=current_accel.hpf_corner,
        desired_odr_index=odr_index,
        desired_range=range_value,
        desired_hpf_corner=hpf_corner,
        applied_odr_index=current_accel.applied_odr_index,
        applied_range=current_accel.applied_range,
        applied_hpf_corner=current_accel.applied_hpf_corner,
        current_state=current_accel.current_state,
        pending_seq=seq,
        applied_seq=current_accel.applied_seq,
        last_ack_at=current_accel.last_ack_at,
        sync_status="pending",
    )

    settings.config[key]["accelerometer"] = to_dict(updated_accel)
    save_settings(settings)
    return updated_accel


# Store an applied ACK later when the node confirms the config.
def apply_accelerometer_config_ack(
    node_id: int,
    odr_index: int,
    range_value: int,
    hpf_corner: int,
    seq_ack: int,
    acked_at: str,
    current_state: str,
):
    settings = load_settings()
    key = str(node_id)

    if key not in settings.config:
        settings.config[key] = build_default_node_config()

    current_raw = settings.config[key].get("accelerometer") or {}
    current_accel = validate_model(AccelerometerConfigModel, current_raw)

    updated_accel = AccelerometerConfigModel(
        odr_index=odr_index,
        range=range_value,
        hpf_corner=hpf_corner,
        desired_odr_index=current_accel.desired_odr_index,
        desired_range=current_accel.desired_range,
        desired_hpf_corner=current_accel.desired_hpf_corner,
        applied_odr_index=odr_index,
        applied_range=range_value,
        applied_hpf_corner=hpf_corner,
        current_state=current_state,
        pending_seq=None,
        applied_seq=seq_ack,
        last_ack_at=acked_at,
        sync_status="synced",
    )

    settings.config[key]["accelerometer"] = to_dict(updated_accel)
    save_settings(settings)
    return updated_accel


# Mark a request as failed if the node rejects it or times out later.
def mark_accelerometer_config_failed(node_id: int):
    settings = load_settings()
    key = str(node_id)

    if key not in settings.config:
        settings.config[key] = build_default_node_config()

    current_raw = settings.config[key].get("accelerometer") or {}
    current_accel = validate_model(AccelerometerConfigModel, current_raw)

    updated_accel = AccelerometerConfigModel(
        odr_index=current_accel.odr_index,
        range=current_accel.range,
        hpf_corner=current_accel.hpf_corner,
        desired_odr_index=current_accel.desired_odr_index,
        desired_range=current_accel.desired_range,
        desired_hpf_corner=current_accel.desired_hpf_corner,
        applied_odr_index=current_accel.applied_odr_index,
        applied_range=current_accel.applied_range,
        applied_hpf_corner=current_accel.applied_hpf_corner,
        current_state=current_accel.current_state,
        pending_seq=current_accel.pending_seq,
        applied_seq=current_accel.applied_seq,
        last_ack_at=current_accel.last_ack_at,
        sync_status="failed",
    )

    settings.config[key]["accelerometer"] = to_dict(updated_accel)
    save_settings(settings)
    return updated_accel