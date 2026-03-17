import socket
import json
import queue
import threading
import time
import csv
import os

HOST = "127.0.0.1"
PORT = 5000

BUFFER_SIZE = 100        # samples in RAM before writing
WRITE_INTERVAL = 0.1       # seconds
DATA_PATH = "/home/pi/Data/accel_data.csv"  # microSD storage

data_buffer = queue.Queue()

# ---------- Socket Receiver ----------
def receive_data():
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.bind((HOST, PORT))
    server.listen(1)
    print("Receiver listening...")

    conn, addr = server.accept()
    print("Connected to generator")

    with conn:
        buffer = ""
        while True:
            chunk = conn.recv(1024).decode()
            if not chunk:
                break

            buffer += chunk
            while "\n" in buffer:
                line, buffer = buffer.split("\n", 1)
                data_buffer.put(json.loads(line))


# ---------- Writer to microSD ----------
def write_to_sd():
    file_exists = os.path.isfile(DATA_PATH)

    while True:
        time.sleep(WRITE_INTERVAL)

        rows = []
        while not data_buffer.empty() and len(rows) < BUFFER_SIZE:
            rows.append(data_buffer.get())

        if rows:
            with open(DATA_PATH, "a", newline="") as f:
                writer = csv.DictWriter(
                    f, fieldnames=["timestamp","node id", "ax", "ay", "az"]
                )
                if not file_exists:
                    writer.writeheader()
                    file_exists = True
                writer.writerows(rows)

            print(f"Wrote {len(rows)} samples to SD")


# ---------- Main ----------
if __name__ == "__main__":
    threading.Thread(target=receive_data, daemon=True).start()
    threading.Thread(target=write_to_sd, daemon=True).start()

    print("Receiver running. Press Ctrl+C to stop.")
    while True:
        time.sleep(1)
