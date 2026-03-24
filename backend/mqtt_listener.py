import json
from datetime import datetime, timezone

import paho.mqtt.client as mqtt

from node_registry import get_node_by_serial
from settings_store import (
    ensure_node_defaults,
    update_accelerometer_runtime_state,
    # apply_accelerometer_config_ack,
    # mark_accelerometer_config_failed,
    # load_settings,
    # apply_node_control_ack,
    # mark_node_control_failed,
)

BROKER_HOST = "localhost"
BROKER_PORT = 1883
STATUS_TOPIC = "wind_turbine/+/status"


# ----------------------------
# Helpers (mapping firmware to backend)
# ----------------------------

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


# ----------------------------
# Core handler
# ----------------------------

def handle_status(topic: str, payload_raw: str):
    try:
        topic_parts = topic.split("/")
        if len(topic_parts) < 3:
            return

        serial = topic_parts[1]
        node = get_node_by_serial(serial, timeout_seconds=300)
        if not node:
            print(f"[MQTT] Unknown node serial: {serial}")
            return

        node_id = node["node_id"]
        ensure_node_defaults(node_id)

        payload = json.loads(payload_raw)

        # ----------------------------
        # Extract fields from node payload
        # ----------------------------
        state = payload.get("state", "unknown")

        # Disable ACK/SEQ handling
        # seq_ack = int(payload.get("seq_ack", 0) or 0)
        # cmd_ack = payload.get("cmd_ack")
        # error_msg = str(payload.get("error", "") or "").strip()

        odr_hz = payload.get("odr_hz")
        range_g = payload.get("range_g")

        acked_at = datetime.now(timezone.utc).isoformat()

        # ----------------------------
        # ALWAYS update runtime state
        # ----------------------------
        update_accelerometer_runtime_state(
            node_id=node_id,
            current_state=state,
            acked_at=acked_at,
        )

        # ==========================================================
        #  CONTROL ACK HANDLING DISABLED
        # ==========================================================
        #
        # settings = load_settings()
        # accel_cfg = settings.config.get(str(node_id), {}).get("accelerometer", {})
        #
        # pending_control_seq = accel_cfg.get("pending_control_seq")
        # pending_control_cmd = accel_cfg.get("pending_control_cmd")
        #
        # if cmd_ack and seq_ack > 0:
        #     if pending_control_seq == seq_ack and pending_control_cmd == cmd_ack:
        #
        #         if error_msg:
        #             print(f"[MQTT] Control command FAILED: {cmd_ack} seq={seq_ack}")
        #             mark_node_control_failed(node_id, error_msg=error_msg)
        #         else:
        #             print(f"[MQTT] Control command ACKED: {cmd_ack} seq={seq_ack}")
        #             apply_node_control_ack(
        #                 node_id=node_id,
        #                 cmd_ack=cmd_ack,
        #                 seq_ack=seq_ack,
        #                 acked_at=acked_at,
        #                 current_state=state,
        #             )

        # ==========================================================
        # CONFIG ACK HANDLING DISABLED
        # ==========================================================
        #
        # pending_seq = accel_cfg.get("pending_seq")
        # desired_hpf = accel_cfg.get("desired_hpf_corner")
        #
        # if seq_ack > 0:
        #     odr_index = map_odr_hz_to_index(odr_hz) if odr_hz else None
        #     range_code = map_range_g_to_code(range_g) if range_g else None
        #
        #     if error_msg:
        #         mark_accelerometer_config_failed(node_id)
        #         return
        #
        #     if pending_seq == seq_ack and odr_index is not None and range_code is not None:
        #         apply_accelerometer_config_ack(
        #             node_id=node_id,
        #             odr_index=odr_index,
        #             range_value=range_code,
        #             hpf_corner=desired_hpf,
        #             seq_ack=seq_ack,
        #             acked_at=acked_at,
        #             current_state=state,
        #         )

    except Exception as e:
        print(f"[MQTT] Error handling status: {e}")


# ----------------------------
# MQTT callbacks
# ----------------------------

def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print("[MQTT] Connected")
        client.subscribe(STATUS_TOPIC)
    else:
        print(f"[MQTT] Connection failed: {rc}")


def on_message(client, userdata, msg):
    payload = msg.payload.decode("utf-8", errors="ignore")
    print(f"[MQTT] {msg.topic} -> {payload}")
    handle_status(msg.topic, payload)


# ----------------------------
# Start function
# ----------------------------

def start_listener():
    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message

    client.connect(BROKER_HOST, BROKER_PORT, 60)
    client.loop_start()

    return client