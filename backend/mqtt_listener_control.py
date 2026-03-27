import json
from datetime import datetime, timezone

import paho.mqtt.client as mqtt

from node_registry import get_node_by_serial, register_serial
from settings_store import (
    ensure_node_defaults,
    update_accelerometer_runtime_state,
)

BROKER_HOST = "localhost"
BROKER_PORT = 1883
STATUS_TOPIC = "wind_turbine/+/status"

# Track live MQTT listener connection health.
MQTT_CONNECTED = False


def map_odr_hz_to_index(odr_hz: int) -> int | None:
    mapping = {
        4000: 0,
        2000: 1,
        1000: 2,
    }
    return mapping.get(int(odr_hz))


def map_range_g_to_code(range_g: int) -> int | None:
    mapping = {
        2: 1,
        4: 2,
        8: 3,
    }
    return mapping.get(int(range_g))


def handle_status(topic: str, payload_raw: str):
    try:
        topic_parts = topic.split("/")
        if len(topic_parts) < 3:
            return

        serial = topic_parts[1].strip()
        if not serial:
            return

        # Status-topic heartbeat: this is what keeps the node online.
        register_serial(serial)

        node = get_node_by_serial(serial, timeout_seconds=300)
        if not node:
            print(f"[MQTT] Unknown node serial: {serial}")
            return

        node_id = node["node_id"]
        ensure_node_defaults(node_id)

        payload = json.loads(payload_raw)

        state = payload.get("state", "unknown")
        odr_hz = payload.get("odr_hz")
        range_g = payload.get("range_g")

        acked_at = datetime.now(timezone.utc).isoformat()

        update_accelerometer_runtime_state(
            node_id=node_id,
            current_state=state,
            acked_at=acked_at,
        )

    except Exception as e:
        print(f"[MQTT] Error handling status: {e}")


def on_connect(client, userdata, flags, rc):
    global MQTT_CONNECTED

    if rc == 0:
        MQTT_CONNECTED = True
        print("[MQTT] Connected")
        client.subscribe(STATUS_TOPIC)
    else:
        MQTT_CONNECTED = False
        print(f"[MQTT] Connection failed: {rc}")


def on_disconnect(client, userdata, rc):
    global MQTT_CONNECTED
    MQTT_CONNECTED = False
    print(f"[MQTT] Disconnected: rc={rc}")


def on_message(client, userdata, msg):
    payload = msg.payload.decode("utf-8", errors="ignore")
    print(f"[MQTT] {msg.topic} -> {payload}")
    handle_status(msg.topic, payload)


def start_listener():
    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message

    client.connect(BROKER_HOST, BROKER_PORT, 60)
    client.loop_start()

    return client