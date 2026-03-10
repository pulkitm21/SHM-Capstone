import json
from pathlib import Path

from settings_schema import (
    DEFAULT_SETTINGS,
    SettingsModel,
    build_default_node_meta,
    build_default_node_config,
    validate_model,
    copy_deep,
    to_dict,
)

DATA_DIR = Path("/storage")
DATA_DIR.mkdir(parents=True, exist_ok=True)

SETTINGS_JSON = DATA_DIR / "settings.json"


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