from datetime import datetime, timezone
from threading import Lock
from typing import Any, Dict, Optional
import math


SENSOR_HEALTH_KEYS = ("accelerometer", "inclinometer", "temperature")

# Shared in-memory cache for live sensor packet health.
_sensor_health_cache: Dict[str, Dict[str, Dict[str, Any]]] = {}
_sensor_health_lock = Lock()


def _utc_now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def _fresh_sensor_state() -> Dict[str, Any]:
    return {
        "last_packet_ts": None,
        "last_valid_data_ts": None,
        "last_nan_ts": None,
        "has_data": False,
        "has_valid_data": False,
        "has_nan_data": False,
        "updated_at": None,
    }


def _fresh_node_state() -> Dict[str, Dict[str, Any]]:
    return {
        "accelerometer": _fresh_sensor_state(),
        "inclinometer": _fresh_sensor_state(),
        "temperature": _fresh_sensor_state(),
    }


def _ensure_node_state(serial: str) -> Dict[str, Dict[str, Any]]:
    state = _sensor_health_cache.get(serial)
    if state is None:
        state = _fresh_node_state()
        _sensor_health_cache[serial] = state
    return state


def _is_nan_value(value: Any) -> bool:
    try:
        return math.isnan(float(value))
    except (TypeError, ValueError):
        return False


def _sensor_has_nan(sensor_payload: Any, sensor_key: str) -> bool:
    # Accelerometer payload is a list of [ts, x, y, z] samples.
    if sensor_key == "accelerometer":
        if not isinstance(sensor_payload, list):
            return False
        for sample in sensor_payload:
            if not isinstance(sample, (list, tuple)) or len(sample) < 4:
                continue
            if any(_is_nan_value(v) for v in sample[1:4]):
                return True
        return False

    # Inclinometer payload is [ts, roll, pitch, yaw].
    if sensor_key == "inclinometer":
        if not isinstance(sensor_payload, (list, tuple)) or len(sensor_payload) < 4:
            return False
        return any(_is_nan_value(v) for v in sensor_payload[1:4])

    # Temperature payload is [ts, value].
    if sensor_key == "temperature":
        if not isinstance(sensor_payload, (list, tuple)) or len(sensor_payload) < 2:
            return False
        return _is_nan_value(sensor_payload[1])

    return False


def _extract_sensor_ts(sensor_payload: Any, sensor_key: str) -> Optional[str]:
    if sensor_key == "accelerometer":
        if isinstance(sensor_payload, list) and len(sensor_payload) > 0:
            first = sensor_payload[0]
            if isinstance(first, (list, tuple)) and len(first) > 0:
                return str(first[0])
        return None

    if sensor_key in ("inclinometer", "temperature"):
        if isinstance(sensor_payload, (list, tuple)) and len(sensor_payload) > 0:
            return str(sensor_payload[0])
        return None

    return None


def update_sensor_health_from_packet(serial: str, packet: Dict[str, Any]) -> None:
    """
    Update live per-sensor packet health from one MQTT data packet.
    """
    if not serial or not isinstance(packet, dict):
        return

    now_iso = _utc_now_iso()

    sensor_payloads = {
        "accelerometer": packet.get("a"),
        "inclinometer": packet.get("i"),
        "temperature": packet.get("T"),
    }

    with _sensor_health_lock:
        node_state = _ensure_node_state(serial)

        for sensor_key, sensor_payload in sensor_payloads.items():
            if sensor_payload is None:
                continue

            sensor_state = node_state[sensor_key]
            packet_ts = _extract_sensor_ts(sensor_payload, sensor_key)
            has_nan = _sensor_has_nan(sensor_payload, sensor_key)

            sensor_state["has_data"] = True
            sensor_state["last_packet_ts"] = packet_ts
            sensor_state["updated_at"] = now_iso

            if has_nan:
                sensor_state["has_nan_data"] = True
                sensor_state["last_nan_ts"] = packet_ts or now_iso
            else:
                sensor_state["has_valid_data"] = True
                sensor_state["last_valid_data_ts"] = packet_ts or now_iso


def get_sensor_health_snapshot(serial: str) -> Dict[str, Dict[str, Any]]:
    """
    Return a copy of the cached sensor health for one node serial.
    """
    with _sensor_health_lock:
        node_state = _sensor_health_cache.get(serial)
        if node_state is None:
            return _fresh_node_state()

        return {
            sensor_key: dict(sensor_state)
            for sensor_key, sensor_state in node_state.items()
        }


def clear_sensor_health(serial: Optional[str] = None) -> None:
    """
    Clear cached health for one node or all nodes.
    """
    with _sensor_health_lock:
        if serial is None:
            _sensor_health_cache.clear()
            return

        _sensor_health_cache.pop(serial, None)