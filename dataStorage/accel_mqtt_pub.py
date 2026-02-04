import time
import math
import random
from datetime import datetime
import paho.mqtt.client as mqtt
import json

#HOST = "127.0.0.1"   # localhost
# Priority order
PI_IPS = [
    "192.168.1.42",   # Pi router-assigned IP (change this)
    "192.168.2.2"    # Pi static direct-Ethernet IP
]
HOST = "127.0.0.1"
PORT = 1883
TOPIC = "shm/windturbine/node01/accel"

client = mqtt.Client()
client.connect(HOST, PORT, keepalive=60)

# Simulation parameters
SAMPLING_FREQ = 200          # Hz
DT = 1.0 / SAMPLING_FREQ
SIM_DURATION = 10            # seconds
NODE_ID = 1

# Vibration parameters (wind turbine-like)
BASE_FREQ = 2.0              # Hz (rotor-related vibration)
AMPLITUDE = 0.5              # g
NOISE_STD = 0.02             # sensor noise (g)

def generate_sample(t):
    """Generate one 3-axis acceleration sample"""
    ax = AMPLITUDE * math.sin(2 * math.pi * BASE_FREQ * t) + random.gauss(0, NOISE_STD)
    ay = AMPLITUDE * math.sin(2 * math.pi * BASE_FREQ * t + math.pi/4) + random.gauss(0, NOISE_STD)
    az = 1.0 + random.gauss(0, NOISE_STD)  # gravity + noise
    return ax, ay, az

def main():
    try:
        print("timestamp,node_id,ax,ay,az")

        start_time = time.time()
        next_sample_time = start_time
        samples = int(SIM_DURATION * SAMPLING_FREQ)

        for i in range(samples):
            now = time.time()
            t = now - start_time

            ax, ay, az = generate_sample(t)

            timestamp = datetime.utcnow().isoformat()
            print(f"{timestamp},{NODE_ID},{ax:.5f},{ay:.5f},{az:.5f}")
            data = {
                "timestamp": timestamp,
                "node id": NODE_ID,
                "ax": ax,
                "ay": ay,
                "az": az
            }
            client.publish(TOPIC, json.dumps(data))

            next_sample_time += DT
            sleep_time = next_sample_time - time.time()
            if sleep_time > 0:
                time.sleep(sleep_time)
    except KeyboardInterrupt:
        print("Generator stopped.")
    finally:
        client.disconnect()

if __name__ == "__main__":
    main()