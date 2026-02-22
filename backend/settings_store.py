import json
from pathlib import Path

# Paths
DATA_DIR = Path("/home/pi/Data")
DATA_DIR.mkdir(parents=True, exist_ok=True)

SETTINGS_JSON = DATA_DIR / "settings.json"

def save_settings(settings) -> None:
    from main import to_dict
    SETTINGS_JSON.write_text(json.dumps(to_dict(settings), indent=2), encoding="utf-8")

def load_settings():
    from main import DEFAULT_SETTINGS, SettingsModel, validate_model, copy_deep

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