import json
import math
import time
from datetime import datetime, timezone
from pathlib import Path
from threading import Lock
from typing import Optional

from settings_store import ensure_node_defaults

NODES_JSON = Path("/home/pi/nodes.json")
NODES_JSON.parent.mkdir(parents=True, exist_ok=True)

if not NODES_JSON.exists():
    NODES_JSON.write_text(json.dumps({"nodes": []}, indent=2), encoding="utf-8")

TOPIC_PREFIX = "wind_turbine"

# Node map X/Y boundaries used by the frontend node map.
MIN_X = 0.06
MAX_X = 0.94
MIN_Y = 0.08
MAX_Y = 0.94

# Tower section boundaries used to determine the node location label.
TOP_SECTION_MAX_Y = 0.33
MIDDLE_SECTION_MAX_Y = 0.66

_SENSOR_RUNTIME_CACHE = {}
_SENSOR_RUNTIME_LOCK = Lock()
_LAST_FLUSH_TIME = 0.0
_FLUSH_INTERVAL = 5.0  # seconds


# Return the current UTC time in ISO format.
def _now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def _fresh_sensor_runtime():
    return {
        "last_packet_ts": None,
        "last_valid_data_ts": None,
        "last_nan_ts": None,
        "updated_at": None,
    }


def _fresh_node_sensor_runtime():
    return {
        "accelerometer": _fresh_sensor_runtime(),
        "inclinometer": _fresh_sensor_runtime(),
        "temperature": _fresh_sensor_runtime(),
    }


# Return True when a value can be parsed as NaN.
def _is_nan_value(value) -> bool:
    try:
        return math.isnan(float(value))
    except (TypeError, ValueError):
        return False


# Normalize inclinometer payloads so both flat and nested packet shapes work.
def _as_sample_list(value):
    if not isinstance(value, list) or not value:
        return []

    if isinstance(value[0], list):
        return value

    return [value]


# Clamp the normalized X position to the valid node map range.
def _clamp_x(value: float) -> float:
    return max(MIN_X, min(MAX_X, float(value)))


# Clamp the normalized Y position to the valid node map range.
def _clamp_y(value: float) -> float:
    return max(MIN_Y, min(MAX_Y, float(value)))


# Determine the tower section label from the node Y position.
def _position_zone_for(y: float) -> str:
    y = _clamp_y(y)

    if y <= TOP_SECTION_MAX_Y:
        return "Top"

    if y <= MIDDLE_SECTION_MAX_Y:
        return "Middle"

    return "Bottom"


# Return a default map position for newly discovered nodes.
def _default_position(node_id: int) -> tuple[float, float]:
    defaults = [
        (0.50, 0.18),
        (0.50, 0.32),
        (0.50, 0.46),
        (0.50, 0.60),
        (0.50, 0.74),
    ]
    return defaults[(node_id - 1) % len(defaults)]


# Save the registry atomically to avoid partial writes.
def _save_registry_raw(data: dict) -> None:
    tmp_path = NODES_JSON.with_suffix(".tmp")
    tmp_path.write_text(json.dumps(data, indent=2), encoding="utf-8")
    tmp_path.replace(NODES_JSON)


# Load and sanitize the raw node registry from disk.
def _load_registry_raw() -> dict:
    if not NODES_JSON.exists():
        return {"nodes": []}

    try:
        raw = json.loads(NODES_JSON.read_text(encoding="utf-8"))
        if not isinstance(raw, dict):
            return {"nodes": []}

        nodes = raw.get("nodes", [])
        if not isinstance(nodes, list):
            return {"nodes": []}

        cleaned = []
        changed = False

        for item in nodes:
            if not isinstance(item, dict):
                continue

            try:
                node_id = int(item["node_id"])
                serial = str(item["serial"]).strip()
                first_seen = str(item["first_seen"])
                last_seen = str(item["last_seen"])

                x = item.get("x")
                y = item.get("y")

                if x is None or y is None:
                    x, y = _default_position(node_id)
                    changed = True
                else:
                    x = _clamp_x(x)
                    y = _clamp_y(y)

                sensor_runtime = item.get("sensor_runtime")
                if not isinstance(sensor_runtime, dict):
                    sensor_runtime = _fresh_node_sensor_runtime()

                cleaned.append(
                    {
                        "node_id": node_id,
                        "serial": serial,
                        "first_seen": first_seen,
                        "last_seen": last_seen,
                        "x": x,
                        "y": y,
                        "sensor_runtime": sensor_runtime,
                    }
                )
            except Exception:
                continue

        cleaned = sorted(cleaned, key=lambda item: item["node_id"])
        cleaned_registry = {"nodes": cleaned}

        if changed:
            _save_registry_raw(cleaned_registry)

        return cleaned_registry

    except Exception:
        return {"nodes": []}


# Build the display label used by the frontend.
def _label_for(node_id: int, serial: str) -> str:
    return f"Node {node_id} - {serial}"


# Parse ISO timestamps safely.
def _parse_iso(ts: str) -> Optional[datetime]:
    try:
        return datetime.fromisoformat(ts.replace("Z", "+00:00"))
    except Exception:
        return None


# Determine whether a node is online from its last_seen timestamp.
def _is_online(last_seen: str, timeout_seconds: int) -> bool:
    dt = _parse_iso(last_seen)
    if dt is None:
        return False

    delta = datetime.now(timezone.utc) - dt.astimezone(timezone.utc)
    return delta.total_seconds() <= timeout_seconds


# Build the frontend-ready node payload with derived fields.
def _build_node_response(item: dict, timeout_seconds: int) -> dict:
    return {
        "node_id": item["node_id"],
        "serial": item["serial"],
        "first_seen": item["first_seen"],
        "last_seen": item["last_seen"],
        "x": item["x"],
        "y": item["y"],
        "position_zone": _position_zone_for(item["y"]),
        "label": _label_for(item["node_id"], item["serial"]),
        "online": _is_online(item["last_seen"], timeout_seconds),
    }


# Return all nodes in a frontend-ready format.
def list_nodes(timeout_seconds: int = 60):
    raw = _load_registry_raw()
    nodes = sorted(raw["nodes"], key=lambda item: item["node_id"])

    out = []
    for item in nodes:
        out.append(_build_node_response(item, timeout_seconds))

    return out


# Return one node by numeric node ID.
def get_node_by_id(node_id: int, timeout_seconds: int = 60):
    for item in list_nodes(timeout_seconds=timeout_seconds):
        if item["node_id"] == node_id:
            return item
    return None


# Return one node by serial number.
def get_node_by_serial(serial: str, timeout_seconds: int = 60):
    serial = serial.strip()
    for item in list_nodes(timeout_seconds=timeout_seconds):
        if item["serial"] == serial:
            return item
    return None


# Register a node serial or refresh its last_seen timestamp.
def register_serial(serial: str, seen_at: Optional[str] = None):
    serial = serial.strip()
    if not serial:
        raise ValueError("serial cannot be empty")

    now_iso = seen_at or _now_iso()

    raw = _load_registry_raw()
    nodes = raw["nodes"]

    for item in nodes:
        if item["serial"] == serial:
            item["last_seen"] = now_iso
            _save_registry_raw({"nodes": sorted(nodes, key=lambda item: item["node_id"])})
            ensure_node_defaults(item["node_id"])
            return _build_node_response(item, 60)

    next_node_id = max((item["node_id"] for item in nodes), default=0) + 1
    default_x, default_y = _default_position(next_node_id)

    new_item = {
        "node_id": next_node_id,
        "serial": serial,
        "first_seen": now_iso,
        "last_seen": now_iso,
        "x": default_x,
        "y": default_y,
        "sensor_runtime": _fresh_node_sensor_runtime(),
    }

    nodes.append(new_item)
    _save_registry_raw({"nodes": sorted(nodes, key=lambda item: item["node_id"])})
    ensure_node_defaults(next_node_id)

    return _build_node_response(new_item, 60)


# Update a node's map position and return the updated node.
def update_node_position(node_id: int, x: float, y: float):
    raw = _load_registry_raw()
    nodes = raw["nodes"]

    for item in nodes:
        if item["node_id"] == node_id:
            item["x"] = _clamp_x(x)
            item["y"] = _clamp_y(y)
            _save_registry_raw({"nodes": sorted(nodes, key=lambda item: item["node_id"])})
            return _build_node_response(item, 60)

    return None


# Update the live per-sensor runtime cache using one decoded MQTT packet.
def update_sensor_runtime(serial: str, packet: dict):
    now_iso = _now_iso()

    with _SENSOR_RUNTIME_LOCK:
        if serial not in _SENSOR_RUNTIME_CACHE:
            _SENSOR_RUNTIME_CACHE[serial] = _fresh_node_sensor_runtime()

        runtime = _SENSOR_RUNTIME_CACHE[serial]

        accel = packet.get("a")
        if isinstance(accel, list) and accel:
            runtime["accelerometer"]["last_packet_ts"] = now_iso

            accel_has_nan = any(
                isinstance(sample, list) and any(_is_nan_value(v) for v in sample[1:4])
                for sample in accel
            )

            if accel_has_nan:
                runtime["accelerometer"]["last_nan_ts"] = now_iso
            else:
                runtime["accelerometer"]["last_valid_data_ts"] = now_iso

        inclin = _as_sample_list(packet.get("i"))
        if inclin:
            runtime["inclinometer"]["last_packet_ts"] = now_iso

            inclin_has_nan = any(
                isinstance(sample, list) and any(_is_nan_value(v) for v in sample[1:])
                for sample in inclin
            )

            if inclin_has_nan:
                runtime["inclinometer"]["last_nan_ts"] = now_iso
            else:
                runtime["inclinometer"]["last_valid_data_ts"] = now_iso

        temp = packet.get("T")
        if isinstance(temp, list) and len(temp) > 1:
            runtime["temperature"]["last_packet_ts"] = now_iso

            if _is_nan_value(temp[1]):
                runtime["temperature"]["last_nan_ts"] = now_iso
            else:
                runtime["temperature"]["last_valid_data_ts"] = now_iso

        for sensor in runtime.values():
            sensor["updated_at"] = now_iso

    _flush_runtime_to_disk()


# Flush the in-memory runtime cache to disk periodically.
def _flush_runtime_to_disk():
    global _LAST_FLUSH_TIME

    now = time.time()
    if now - _LAST_FLUSH_TIME < _FLUSH_INTERVAL:
        return

    raw = _load_registry_raw()

    for node in raw["nodes"]:
        serial = node["serial"]
        if serial in _SENSOR_RUNTIME_CACHE:
            node["sensor_runtime"] = _SENSOR_RUNTIME_CACHE[serial]

    _save_registry_raw(raw)
    _LAST_FLUSH_TIME = now


# Return one node's sensor runtime, preferring the in-memory cache.
def get_sensor_runtime(serial: str):
    with _SENSOR_RUNTIME_LOCK:
        cached = _SENSOR_RUNTIME_CACHE.get(serial)
        if isinstance(cached, dict):
            return json.loads(json.dumps(cached))

    raw = _load_registry_raw()

    for node in raw["nodes"]:
        if node["serial"] == serial:
            runtime = node.get("sensor_runtime", {})
            if isinstance(runtime, dict):
                return runtime
            return {}

    return {}


# Extract the node serial number from the MQTT topic.
def serial_from_topic(topic: str) -> str:
    parts = topic.split("/")
    if len(parts) < 3:
        raise ValueError(f"Unexpected MQTT topic: {topic}")

    if parts[0] != TOPIC_PREFIX:
        raise ValueError(f"Unexpected topic prefix: {topic}")

    serial = parts[1].strip()
    if not serial:
        raise ValueError(f"Missing serial in topic: {topic}")

    return serial