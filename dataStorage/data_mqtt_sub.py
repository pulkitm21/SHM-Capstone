import json
import paho.mqtt.client as mqtt
from collections import deque

BROKER_IP = "localhost"
PORT = 1883
TOPIC = "shm/windturbine/+/accel"

BUFFER_SIZE = 2000
buffer = deque(maxlen=BUFFER_SIZE)

def on_connect(client, userdata, flags, rc):
    print("Connected to broker")
    client.subscribe(TOPIC)

def on_message(client, userdata, msg):
    data = json.loads(msg.payload.decode())
    buffer.append(data)

    # Simple file write (microSD)
    with open("/home/pi/accel_data.csv", "a") as f:
        f.write(f"{data['timestamp']},{data['ax']},{data['ay']},{data['az']}\n")

client = mqtt.Client()
client.on_connect = on_connect
client.on_message = on_message

client.connect(BROKER_IP, PORT, keepalive=60)
client.loop_forever()
