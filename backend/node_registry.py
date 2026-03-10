import json
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

from settings_store import ensure_node_defaults

DATA_DIR = Path("/mnt/ssd")
DATA_DIR.mkdir(parents=True, exist_ok=True)

NODES_JSON = DATA_DIR / "nodes.json"
TOPIC_PREFIX = "wind_turbine"

#this is for testing purposes. remove after tests
TEST_SERIAL = "TEST123"


def _now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def _save_registry_raw(data: dict) -> None:
    NODES_JSON.write_text(json.dumps(data, indent=2), encoding="utf-8")


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
        for item in nodes:
            if not isinstance(item, dict):
                continue

            try:
                cleaned.append(
                    {
                        "node_id": int(item["node_id"]),
                        "serial": str(item["serial"]).strip(),
                        "first_seen": str(item["first_seen"]),
                        "last_seen": str(item["last_seen"]),
                    }
                )
            except Exception:
                continue

        return {"nodes": cleaned}
    except Exception:
        return {"nodes": []}


def _label_for(node_id: int, serial: str) -> str:
    return f"Node {node_id} - {serial}"


def _parse_iso(ts: str) -> Optional[datetime]:
    try:
        return datetime.fromisoformat(ts.replace("Z", "+00:00"))
    except Exception:
        return None


def _is_online(last_seen: str, timeout_seconds: int) -> bool:
    dt = _parse_iso(last_seen)
    if dt is None:
        return False

    delta = datetime.now(timezone.utc) - dt.astimezone(timezone.utc)
    return delta.total_seconds() <= timeout_seconds


#this is for testing purposes. remove after tests
def _ensure_test_node():
    raw = _load_registry_raw()
    nodes = raw["nodes"]

    for item in nodes:
        if item["serial"] == TEST_SERIAL:
            ensure_node_defaults(item["node_id"])
            return

    now_iso = _now_iso()
    next_node_id = max((item["node_id"] for item in nodes), default=0) + 1

    new_item = {
        "node_id": next_node_id,
        "serial": TEST_SERIAL,
        "first_seen": now_iso,
        "last_seen": now_iso,
    }

    nodes.append(new_item)
    _save_registry_raw({"nodes": sorted(nodes, key=lambda x: x["node_id"])})
    ensure_node_defaults(next_node_id)


def list_nodes(timeout_seconds: int = 60):
    raw = _load_registry_raw()
    nodes = sorted(raw["nodes"], key=lambda x: x["node_id"])

    out = []
    for item in nodes:
        out.append(
            {
                "node_id": item["node_id"],
                "serial": item["serial"],
                "first_seen": item["first_seen"],
                "last_seen": item["last_seen"],
                "label": _label_for(item["node_id"], item["serial"]),
                "online": _is_online(item["last_seen"], timeout_seconds),
            }
        )

    return out


def get_node_by_id(node_id: int, timeout_seconds: int = 60):
    for item in list_nodes(timeout_seconds=timeout_seconds):
        if item["node_id"] == node_id:
            return item
    return None


def get_node_by_serial(serial: str, timeout_seconds: int = 60):
    serial = serial.strip()
    for item in list_nodes(timeout_seconds=timeout_seconds):
        if item["serial"] == serial:
            return item
    return None


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
            _save_registry_raw({"nodes": sorted(nodes, key=lambda x: x["node_id"])})
            ensure_node_defaults(item["node_id"])
            return {
                "node_id": item["node_id"],
                "serial": item["serial"],
                "first_seen": item["first_seen"],
                "last_seen": item["last_seen"],
                "label": _label_for(item["node_id"], item["serial"]),
                "online": True,
            }

    next_node_id = max((item["node_id"] for item in nodes), default=0) + 1

    new_item = {
        "node_id": next_node_id,
        "serial": serial,
        "first_seen": now_iso,
        "last_seen": now_iso,
    }

    nodes.append(new_item)
    _save_registry_raw({"nodes": sorted(nodes, key=lambda x: x["node_id"])})
    ensure_node_defaults(next_node_id)

    return {
        "node_id": new_item["node_id"],
        "serial": new_item["serial"],
        "first_seen": new_item["first_seen"],
        "last_seen": new_item["last_seen"],
        "label": _label_for(new_item["node_id"], new_item["serial"]),
        "online": True,
    }


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


#this is for testing purposes. remove after tests
_ensure_test_node()