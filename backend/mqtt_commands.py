# Publish Pi-side MQTT commands for node configuration and control.
import json
from typing import Literal

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
def publish_accelerometer_config(
    serial: str,
    odr_index: int,
    range_value: int,
    hpf_corner: int,
    seq: int,
) -> None:
    payload = {
        "odr_index": odr_index,
        "range": range_value,
        "hpf_corner": hpf_corner,
        "seq": seq,
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
def publish_node_control(
    serial: str,
    cmd: Literal["start", "stop", "init", "reset"],
) -> None:
    payload = {"cmd": cmd}

    mqtt_publish.single(
        topic=control_topic(serial),
        payload=json.dumps(payload),
        hostname=BROKER_HOST,
        port=BROKER_PORT,
        qos=MQTT_QOS,
        retain=False,
    )