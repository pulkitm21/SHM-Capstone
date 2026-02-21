import json
import os
import struct
from datetime import datetime
import paho.mqtt.client as mqtt

BROKER_IP = "localhost"
PORT = 1883
TOPIC = "wind_turbine/+"
DATA_DIR = "/home/pi/Data"

os.makedirs(DATA_DIR, exist_ok=True)

# Binary formats
ACCEL_FORMAT = "<dfff"
INCLIN_FORMAT = "<dfff"
TEMP_FORMAT = "<df"

# Track current date
current_date_str = None


def get_daily_filenames(node_id):
    date_str = datetime.now().strftime("%Y%m%d")

    accel_file = os.path.join(DATA_DIR, f"accel_{node_id}_{date_str}.bin")
    inclin_file = os.path.join(DATA_DIR, f"inclin_{node_id}_{date_str}.bin")
    temp_file = os.path.join(DATA_DIR, f"temp_{node_id}_{date_str}.bin")

    return date_str, accel_file, inclin_file, temp_file


def on_connect(client, userdata, flags, rc):
    print("Connected to broker")
    client.subscribe(TOPIC)


def on_message(client, userdata, msg):
    global current_date_str

    # Extract node ID from topic
    # Example: wind_turbine/node2 → node2
    topic_parts = msg.topic.split("/")
    if len(topic_parts) != 2:
        return

    node_id = topic_parts[1]

    data = json.loads(msg.payload.decode())

    today_str, accel_path, inclin_path, temp_path = get_daily_filenames(node_id)

    if today_str != current_date_str:
        current_date_str = today_str
        print(f"Switched to new daily files for {current_date_str}")

    timestamp = float(data["t"])

    # --- Acceleration ---
    if "a" in data:
        packet = struct.pack(
            ACCEL_FORMAT,
            timestamp,
            float(data["a"][0]),
            float(data["a"][1]),
            float(data["a"][2]),
        )
        with open(accel_path, "ab") as f:
            f.write(packet)

    # --- Inclinometer ---
    if "i" in data:
        packet = struct.pack(
            INCLIN_FORMAT,
            timestamp,
            float(data["i"][0]),
            float(data["i"][1]),
            float(data["i"][2]),
        )
        with open(inclin_path, "ab") as f:
            f.write(packet)

    # --- Temperature ---
    if "T" in data:
        packet = struct.pack(
            TEMP_FORMAT,
            timestamp,
            float(data["t"]),
        )
        with open(temp_path, "ab") as f:
            f.write(packet)


client = mqtt.Client()
client.on_connect = on_connect
client.on_message = on_message

client.connect(BROKER_IP, PORT, 60)
client.loop_forever()