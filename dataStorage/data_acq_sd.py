import socket
import os
from datetime import datetime

# Configuration
HOST = ""                  # Listen on all interfaces
PORT = 9000
DATA_DIR = "/home/pi/data"
FILENAME = "accel_data.csv"

# Ensure storage directory exists (microSD)
os.makedirs(DATA_DIR, exist_ok=True)
file_path = os.path.join(DATA_DIR, FILENAME)

def main():
    # Create TCP socket
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.bind((HOST, PORT))
    server.listen(1)

    print(f"Listening for data on port {PORT}...")

    conn, addr = server.accept()
    print(f"Connected by {addr}")

    with open(file_path, "a") as f:
        # Write CSV header once
        if f.tell() == 0:
            f.write("timestamp,node_id,ax,ay,az\n")

        try:
            while True:
                data = conn.recv(1024)
                if not data:
                    break

                # Decode incoming CSV line(s)
                decoded = data.decode("utf-8")

                # Optional: add receive timestamp
                for line in decoded.strip().split("\n"):
                    f.write(line + "\n")

                f.flush()  # ensure data is written to microSD

        except KeyboardInterrupt:
            print("Stopping receiver...")

    conn.close()
    server.close()

if __name__ == "__main__":
    main()
