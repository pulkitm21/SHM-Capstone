import json
import os
import struct
import paho.mqtt.client as mqtt
from datetime import datetime

BROKER_IP = "localhost"
PORT = 1883
TOPIC = "wind_turbine/data"
DATA_DIR = "/home/pi/Data"

current_date_str = None
current_file_path = None

# Define binary packet format
# d = float64 (timestamp)
# f = float32 (ax, ay, az)
PACKET_FORMAT = "<dfff"
PACKET_SIZE = struct.calcsize(PACKET_FORMAT)


def get_daily_filename():
    date_str = datetime.now().strftime("%Y%m%d")
    filename = f"accel_data_{date_str}.bin"
    return date_str, os.path.join(DATA_DIR, filename)


def on_connect(client, userdata, flags, rc):
    print("Connected to broker")
    client.subscribe(TOPIC)


def on_message(client, userdata, msg):
    global current_date_str, current_file_path

    data = json.loads(msg.payload.decode())

    today_str, file_path = get_daily_filename()

    if today_str != current_date_str:
        current_date_str = today_str
        current_file_path = file_path
        print(f"Switched to new daily binary file: {current_file_path}")

    # Pack into binary
    packet = struct.pack(
        PACKET_FORMAT,
        float(data["timestamp"]),
        float(data["ax"]),
        float(data["ay"]),
        float(data["az"]),
    )

    with open(current_file_path, "ab") as f:
        f.write(packet)


client = mqtt.Client()
client.on_connect = on_connect
client.on_message = on_message
client.connect(BROKER_IP, PORT, 60)
client.loop_forever()