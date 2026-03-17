import json
import os
import struct
import sys
from pathlib import Path
from datetime import datetime

import paho.mqtt.client as mqtt

BACKEND_DIR = Path(__file__).resolve().parents[1] / "backend"
sys.path.insert(0, str(BACKEND_DIR))

from backend.node_registry import register_serial, serial_from_topic  # noqa: E402


BROKER_IP = "localhost"
PORT = 1883
TOPICS = [
    ("wind_turbine/+/data", 0),
    ("wind_turbine/+/status", 0),
]
DATA_DIR = "/home/pi/Data"

current_date_str = None
current_file_path = None

PACKET_FORMAT = "<dfff"
PACKET_SIZE = struct.calcsize(PACKET_FORMAT)


def get_daily_filename():
    date_str = datetime.now().strftime("%Y%m%d")
    filename = f"accel_data_{date_str}.bin"
    return date_str, os.path.join(DATA_DIR, filename)


def on_connect(client, userdata, flags, reason_code, properties=None):
    print("Connected with result code", reason_code)
    for topic, qos in TOPICS:
        client.subscribe(topic, qos=qos)


def on_message(client, userdata, msg):
    global current_date_str, current_file_path

    try:
        serial = serial_from_topic(msg.topic)
        node_info = register_serial(serial)
    except Exception as exc:
        print(f"Topic parse / registry update failed for {msg.topic}: {exc}")
        return

    if msg.topic.endswith("/status"):
        return

    try:
        data = json.loads(msg.payload.decode())
    except Exception as exc:
        print(f"JSON decode failed for {msg.topic}: {exc}")
        return

    today_str, file_path = get_daily_filename()

    if today_str != current_date_str:
        current_date_str = today_str
        current_file_path = file_path
        print(f"Switched to new daily binary file: {current_file_path}")

    try:
        packet = struct.pack(
            PACKET_FORMAT,
            float(data["timestamp"]),
            float(data["ax"]),
            float(data["ay"]),
            float(data["az"]),
        )
    except Exception as exc:
        print(f"Binary pack failed for {node_info['label']}: {exc}")
        return

    with open(current_file_path, "ab") as f:
        f.write(packet)


client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
client.on_connect = on_connect
client.on_message = on_message
client.connect(BROKER_IP, PORT, 60)
client.loop_forever()