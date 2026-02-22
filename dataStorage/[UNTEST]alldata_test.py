import json
import os
import struct
from datetime import datetime
import paho.mqtt.client as mqtt

BROKER_IP = "localhost"
PORT = 1883
TOPIC = "wind_turbine/data"
DATA_DIR = "/home/pi/Data"

# Ensure directory exists
os.makedirs(DATA_DIR, exist_ok=True)

# Binary formats
ACCEL_FORMAT = "<dfff"
INCLIN_FORMAT = "<dfff"
TEMP_FORMAT = "<df"

ACCEL_SIZE = struct.calcsize(ACCEL_FORMAT)
INCLIN_SIZE = struct.calcsize(INCLIN_FORMAT)
TEMP_SIZE = struct.calcsize(TEMP_FORMAT)

current_date_str = None


def get_daily_filenames():
    date_str = datetime.now().strftime("%Y%m%d")

    accel_file = os.path.join(DATA_DIR, f"accel_{date_str}.bin")
    inclin_file = os.path.join(DATA_DIR, f"inclin_{date_str}.bin")
    temp_file = os.path.join(DATA_DIR, f"temp_{date_str}.bin")

    return date_str, accel_file, inclin_file, temp_file


def on_connect(client, userdata, flags, rc):
    print("Connected to broker")
    client.subscribe(TOPIC)


def on_message(client, userdata, msg):
    global current_date_str

    data = json.loads(msg.payload.decode())

    today_str, accel_path, inclin_path, temp_path = get_daily_filenames()

    if today_str != current_date_str:
        current_date_str = today_str
        print(f"Switched to new daily files for {current_date_str}")

    timestamp = float(data["t"])

    # --- Acceleration ---
    if "a" in data:
        accel_packet = struct.pack(
            ACCEL_FORMAT,
            timestamp,
            float(data["a"][0]),
            float(data["a"][1]),
            float(data["a"][2]),
        )
        with open(accel_path, "ab") as f:
            f.write(accel_packet)

    # --- Inclinometer ---
    if "i" in data:
        inclin_packet = struct.pack(
            INCLIN_FORMAT,
            timestamp,
            float(data["i"][0]),
            float(data["i"][1]),
            float(data["i"][2]),
        )
        with open(inclin_path, "ab") as f:
            f.write(inclin_packet)

    # --- Temperature ---
    if "T" in data:
        temp_packet = struct.pack(
            TEMP_FORMAT,
            timestamp,
            float(data["T"]),
        )
        with open(temp_path, "ab") as f:
            f.write(temp_packet)


client = mqtt.Client()
client.on_connect = on_connect
client.on_message = on_message

client.connect(BROKER_IP, PORT, 60)
client.loop_forever()