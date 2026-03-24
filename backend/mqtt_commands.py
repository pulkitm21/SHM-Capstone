# Publish Pi-side MQTT commands for node configuration and control.
import json

import paho.mqtt.publish as mqtt_publish

BROKER_HOST = "localhost"
BROKER_PORT = 1883
MQTT_QOS = 1


# Build the accelerometer configure topic for a node serial.
def configure_topic(serial: str) -> str:
    return f"wind_turbine/{serial}/cmd/configure"


# Build the control topic for a node serial.
def control_topic(serial: str) -> str:
    return f"wind_turbine/{serial}/cmd/control"


# Publish an accelerometer configuration command to the target node.
# seq/ack support is intentionally disabled for now.
def publish_accelerometer_config(
    serial: str,
    odr_index: int,
    range_value: int,
    hpf_corner: int,
    # seq: int,
) -> None:
    payload = {
        "odr_index": odr_index,
        "range": range_value,
        "hpf_corner": hpf_corner,
        # "seq": seq,  # Disabled 
    }

    mqtt_publish.single(
        topic=configure_topic(serial),
        payload=json.dumps(payload),
        hostname=BROKER_HOST,
        port=BROKER_PORT,
        qos=MQTT_QOS,
        retain=False,
    )


# Publish a runtime control command to the target node.
# publish only the command and do not include seq.
def publish_node_control(
    serial: str,
    cmd: str,
    # seq: int | None = None,
) -> None:
    payload = {
        "cmd": cmd,
    }

    # Old Pass 2 logic kept here for later:
    # if seq is not None:
    #     payload["seq"] = seq

    mqtt_publish.single(
        topic=control_topic(serial),
        payload=json.dumps(payload),
        hostname=BROKER_HOST,
        port=BROKER_PORT,
        qos=MQTT_QOS,
        retain=False,
    )