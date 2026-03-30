import json

import paho.mqtt.client as mqtt
from node_registry import update_sensor_runtime
from fault_logger import log_fault_events
from node_registry import register_serial, serial_from_topic, get_node_by_serial
from raw_backup import write_raw, close_all as raw_backup_close_all
from settings_store import (
    apply_accelerometer_config_ack,
    update_accelerometer_runtime_state,
)

from encoder_storage import (
    enqueue_packet,
    normalise_sensor_timestamps,
    now_iso,
    start_consumer_thread,
)

BROKER_IP = "localhost"
PORT = 1883
TOPIC = "wind_turbine/+/data"
STATUS_TOPIC = "wind_turbine/+/status"
FAULT_TOPIC = "wind_turbine/+/faults"
KEEPALIVE_S = 60
MAX_RECONNECT_DELAY_S = 30


def handle_status_message(topic: str, payload_bytes: bytes) -> None:
    """Process node status messages and update backend config/runtime state."""
    try:
        serial = serial_from_topic(topic)
        node = get_node_by_serial(serial, timeout_seconds=60)
        if node is None:
            print(f"[status] Node not found for serial {serial}")
            return

        payload = json.loads(payload_bytes.decode())
        current_state = str(payload.get("state") or "unknown")
        acked_at = now_iso()

        seq_ack = payload.get("seq_ack")
        applied = bool(payload.get("applied"))
        has_full_config = all(key in payload for key in ("odr_index", "range", "hpf_corner"))

        if applied and seq_ack is not None and has_full_config:
            apply_accelerometer_config_ack(
                node_id=node["node_id"],
                odr_index=int(payload["odr_index"]),
                range_value=int(payload["range"]),
                hpf_corner=int(payload["hpf_corner"]),
                seq_ack=int(seq_ack),
                acked_at=acked_at,
                current_state=current_state,
            )
            print(f"[status] Applied config ACK for {serial} seq={seq_ack}")
            return

        update_accelerometer_runtime_state(
            node_id=node["node_id"],
            current_state=current_state,
            acked_at=acked_at,
        )
        print(f"[status] Updated runtime state for {serial}: {current_state}")
    except Exception as e:
        print(f"Failed to process status message on {topic}: {e}")


def handle_fault_message(topic: str, payload_bytes: bytes) -> None:
    """Process fault messages from wind_turbine/{serial}/faults."""
    try:
        serial = serial_from_topic(topic)
        register_serial(serial)
        data = json.loads(payload_bytes.decode())

        ts = data.get("ts")
        faults = data.get("f")
        if ts is None or faults is None:
            print(f"[faults] Invalid payload from {serial}: {data}")
            return

        if isinstance(faults, int):
            faults = [faults]
        elif not isinstance(faults, list):
            print(f"[faults] Invalid fault format from {serial}: {faults}")
            return

        fault_events = [(int(code), ts) for code in faults]
        log_fault_events(serial_number=serial, fault_events=fault_events)
    except Exception as e:
        print(f"Error processing fault message on {topic}: {e}")


def on_connect(client, userdata, flags, reason_code, properties):
    """Subscribe to data, status, and fault topics on successful MQTT connect."""
    if reason_code == 0:
        print("[MQTT] Connected to broker")
    else:
        print(f"[MQTT] Connection failed: reason_code={reason_code} — will retry automatically")
        return

    client.subscribe(TOPIC)
    client.subscribe(STATUS_TOPIC)
    client.subscribe(FAULT_TOPIC)


def on_disconnect(client, userdata, flags, reason_code, properties):
    """Log disconnect events. Paho handles reconnect backoff."""
    if reason_code == 0:
        print("[MQTT] Disconnected cleanly")
    else:
        print(
            f"[MQTT] Unexpected disconnect (reason_code={reason_code}) — "
            f"paho will reconnect automatically with exponential back-off"
        )


def on_message(client, userdata, msg):
    """Handle incoming MQTT packets for status updates, fault events, and sensor data."""
    try:
        if msg.topic.endswith("/status"):
            handle_status_message(msg.topic, msg.payload)
            return

        if msg.topic.endswith("/faults"):
            handle_fault_message(msg.topic, msg.payload)
            return

        if not msg.topic.endswith("/data"):
            return

        topic_parts = msg.topic.split("/")
        node_id = topic_parts[1]

        register_serial(node_id)
        write_raw(node_id, msg.payload)

        data = json.loads(msg.payload.decode())

        if not normalise_sensor_timestamps(data, node_id):
            return
        
        update_sensor_runtime(node_id, data)

        enqueue_packet(node_id, data)
    except Exception as e:
        print(f"Error processing MQTT message on {msg.topic}: {e}")


def main():
    start_consumer_thread()

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message
    client.reconnect_delay_set(min_delay=1, max_delay=MAX_RECONNECT_DELAY_S)

    try:
        client.connect(BROKER_IP, PORT, keepalive=KEEPALIVE_S)
        client.loop_forever(retry_first_connection=True)
    except Exception as e:
        print(f"[MQTT] Fatal connection error: {e}")
        raise
    finally:
        raw_backup_close_all()

if __name__ == "__main__":
    main()


