"""
sim_sensor.py
-------------
Simulated wind-turbine sensor node publisher for testing.
Mimics the exact JSON packet format consumed by multiple_nodes_test.py.

Packet format (JSON over MQTT):
  topic : wind_turbine/<node_id>/data
  payload: {
      "a": [[t,x,y,z], ...],   # Accelerometer — each sample carries its own timestamp
      "i": [t,roll,pitch,yaw], # Inclinometer  — single reading with timestamp
      "T": [t,val]             # Temperature   — value with timestamp
  }
  Each sensor carries its own Unix timestamp (seconds) so sensors
  sampled at different rates have independent time axes.

Usage:
    # Single node, all sensors, default rate
    python sim_sensor.py

    # Two nodes simultaneously, 10 Hz accel, 1 Hz inclin+temp
    python sim_sensor.py --nodes node1 node2 --accel-hz 10 --slow-hz 1

    # Suppress a sensor type to test partial packets
    python sim_sensor.py --no-temp

    # Connect to a remote broker
    python sim_sensor.py --broker 192.168.1.50 --port 1883
"""

import argparse
import json
import math
import random
import threading
import time

import paho.mqtt.client as mqtt

# ── Defaults ──────────────────────────────────────────────────────
DEFAULT_BROKER    = "192.168.2.2"
DEFAULT_PORT      = 1883
DEFAULT_NODES     = ["node1"]
DEFAULT_ACCEL_HZ  = 10      # accelerometer publish rate
DEFAULT_SLOW_HZ   = 1       # inclinometer + temperature publish rate
DEFAULT_ACCEL_SPB = 10       # accel samples per MQTT packet (burst)

TOPIC_TEMPLATE = "wind_turbine/{node_id}/data"


# ──────────────────────────────────────────────────────────────────
# Physics simulation — simple, plausible waveforms
# ──────────────────────────────────────────────────────────────────

class SensorSimulator:
    """Generates realistic-looking (but synthetic) sensor readings."""

    def __init__(self, node_id: str, seed: int = None):
        self.node_id  = node_id
        rng_seed      = seed if seed is not None else hash(node_id) & 0xFFFFFFFF
        self.rng      = random.Random(rng_seed)
        self.start_t  = time.time()

        # Each node gets slightly different natural frequencies
        self.vib_freq   = 2.0  + self.rng.uniform(-0.3, 0.3)   # Hz — blade vibration
        self.sway_freq  = 0.05 + self.rng.uniform(-0.01, 0.01) # Hz — tower sway
        self.temp_base  = 25.0 + self.rng.uniform(-5.0, 5.0)   # °C ambient

    def _elapsed(self) -> float:
        return time.time() - self.start_t

    def accel_samples(self, n: int, hz: float) -> list[list[float]]:
        """Return n accelerometer samples each as [t, x, y, z].
        Timestamps are evenly back-spaced so the last sample lands at now."""
        samples = []
        now = time.time()
        dt  = 1.0 / hz if n > 1 else 0.0
        for i in range(n):
            t       = round(now - (n - 1 - i) * dt, 6)
            elapsed = t - self.start_t

            # Gravity component (near-static tilt ≈ 0.02 g sway)
            g_x = 0.02 * math.sin(2 * math.pi * self.sway_freq * elapsed)
            g_z = math.sqrt(max(0, 1.0 - g_x**2))

            # Vibration on all axes
            vib_amp = 0.05
            vib     = vib_amp * math.sin(2 * math.pi * self.vib_freq * elapsed)
            noise   = lambda: self.rng.gauss(0, 0.002)

            x = round(g_x + vib * 0.6 + noise(), 4)
            y = round(       vib * 0.4 + noise(), 4)
            z = round(g_z  + vib * 0.1 + noise(), 4)
            samples.append([t, x, y, z])   # ← timestamp first
        return samples

    def inclin(self) -> list[float]:
        """Return [t, roll, pitch, yaw] with the current timestamp."""
        t       = round(time.time(), 6)
        elapsed = self._elapsed()
        roll  = round(1.5  * math.sin(2 * math.pi * self.sway_freq * elapsed)
                      + self.rng.gauss(0, 0.01), 4)
        pitch = round(0.8  * math.cos(2 * math.pi * self.sway_freq * elapsed * 0.9)
                      + self.rng.gauss(0, 0.01), 4)
        yaw   = round(0.05 * elapsed % 360 + self.rng.gauss(0, 0.005), 4)  # slow drift
        return [t, roll, pitch, yaw]   # ← timestamp first

    def temperature(self) -> list:
        """Return [t, val] with the current timestamp."""
        t       = round(time.time(), 6)
        elapsed = self._elapsed()
        drift   = 0.3 * math.sin(2 * math.pi * elapsed / 3600.0)  # 1-hour cycle
        val     = round(self.temp_base + drift + self.rng.gauss(0, 0.05), 2)
        return [t, val]   # ← timestamp first


# ──────────────────────────────────────────────────────────────────
# Publisher thread — one per node
# ──────────────────────────────────────────────────────────────────

class NodePublisher(threading.Thread):

    def __init__(self,
                 node_id: str,
                 client: mqtt.Client,
                 accel_hz: float,
                 slow_hz: float,
                 samples_per_burst: int,
                 enable_accel: bool,
                 enable_inclin: bool,
                 enable_temp: bool):
        super().__init__(daemon=True)
        self.node_id     = node_id
        self.client      = client
        self.accel_hz    = accel_hz
        self.slow_hz     = slow_hz
        self.spb         = samples_per_burst
        self.enable_accel  = enable_accel
        self.enable_inclin = enable_inclin
        self.enable_temp   = enable_temp
        self.sim         = SensorSimulator(node_id)
        self.topic       = TOPIC_TEMPLATE.format(node_id=node_id)
        self._stop_event = threading.Event()

        self.accel_interval = 1.0 / accel_hz

    def stop(self):
        self._stop_event.set()

    def run(self):
        # Use accumulating next_tick instead of last_tick = now to prevent
        # drift: each deadline is scheduled relative to the previous one,
        # not relative to when the loop body actually finished executing.
        next_tick = time.perf_counter()
        sent      = 0

        print(f"[{self.node_id}] Publisher started → topic: {self.topic}")

        while not self._stop_event.is_set():
            now_pc = time.perf_counter()

            if now_pc >= next_tick:
                # Advance deadline by exactly one interval regardless of
                # how late we woke up — keeps long-term rate accurate.
                next_tick += self.accel_interval

                # Every packet always contains all enabled sensor types.
                # Each sensor independently timestamps its own reading.
                packet = {}

                if self.enable_accel:
                    packet["a"] = self.sim.accel_samples(self.spb, self.accel_hz)
                if self.enable_inclin:
                    packet["i"] = self.sim.inclin()
                if self.enable_temp:
                    packet["T"] = self.sim.temperature()

                payload = json.dumps(packet)
                self.client.publish(self.topic, payload, qos=0)
                sent += 1

                if sent <= 3 or sent % 50 == 0:
                    _preview(self.node_id, sent, packet)

            # Sleep for a fraction of the interval so we poll frequently
            # enough not to miss a deadline, without busy-waiting.
            # At 10 Hz (interval=0.1 s) this sleeps ~10 ms per poll.
            sleep_s = min(self.accel_interval * 0.1, next_tick - time.perf_counter())
            if sleep_s > 0:
                time.sleep(sleep_s)


# ──────────────────────────────────────────────────────────────────
# Helpers
# ──────────────────────────────────────────────────────────────────

def _preview(node_id: str, seq: int, packet: dict):
    # Use the timestamp from the first available sensor for the log line
    ts = None
    if "a" in packet and packet["a"]:
        ts = packet["a"][-1][0]
    elif "i" in packet:
        ts = packet["i"][0]
    elif "T" in packet:
        ts = packet["T"][0]

    dt  = time.strftime("%H:%M:%S", time.localtime(ts)) if ts else "?"
    msg = f"[{node_id}] #{seq:>5}  ({dt})"

    if "a" in packet:
        n    = len(packet["a"])
        last = packet["a"][-1]   # [t, x, y, z]
        msg += f"  accel[{n}] t={last[0]:.3f} last=({last[1]:+.3f},{last[2]:+.3f},{last[3]:+.3f})g"
    if "i" in packet:
        i = packet["i"]          # [t, roll, pitch, yaw]
        msg += f"  inclin t={i[0]:.3f} ({i[1]:+.3f},{i[2]:+.3f},{i[3]:+.3f})°"
    if "T" in packet:
        msg += f"  temp t={packet['T'][0]:.3f} {packet['T'][1]:.2f}°C"
    print(msg)


def on_connect(client, userdata, flags, reason_code, properties):
    if reason_code == 0:
        print(f"✓ Connected to broker")
    else:
        print(f"✗ Connection failed: reason_code={reason_code}")


def on_disconnect(client, userdata, flags, reason_code, properties):
    print(f"Disconnected (reason_code={reason_code})")


# ──────────────────────────────────────────────────────────────────
# CLI
# ──────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Simulated wind-turbine MQTT sensor publisher"
    )
    parser.add_argument("--broker",     default=DEFAULT_BROKER,
                        help=f"MQTT broker IP/hostname (default: {DEFAULT_BROKER})")
    parser.add_argument("--port",       type=int, default=DEFAULT_PORT,
                        help=f"MQTT broker port (default: {DEFAULT_PORT})")
    parser.add_argument("--nodes",      nargs="+", default=DEFAULT_NODES,
                        metavar="NODE_ID",
                        help="One or more node IDs to simulate (default: node1)")
    parser.add_argument("--accel-hz",   type=float, default=DEFAULT_ACCEL_HZ,
                        help=f"Accelerometer publish rate Hz (default: {DEFAULT_ACCEL_HZ})")
    parser.add_argument("--slow-hz",    type=float, default=DEFAULT_SLOW_HZ,
                        help=f"Inclin + temp publish rate Hz (default: {DEFAULT_SLOW_HZ})")
    parser.add_argument("--samples",    type=int, default=DEFAULT_ACCEL_SPB,
                        metavar="N",
                        help=f"Accel samples per packet burst (default: {DEFAULT_ACCEL_SPB})")
    parser.add_argument("--no-accel",   action="store_true", help="Disable accelerometer")
    parser.add_argument("--no-inclin",  action="store_true", help="Disable inclinometer")
    parser.add_argument("--no-temp",    action="store_true", help="Disable temperature")
    parser.add_argument("--duration",   type=float, default=0,
                        metavar="SECONDS",
                        help="Run for N seconds then exit (0 = run forever)")
    args = parser.parse_args()

    print(f"Sim sensor publisher")
    print(f"  Broker  : {args.broker}:{args.port}")
    print(f"  Nodes   : {args.nodes}")
    print(f"  Accel   : {'OFF' if args.no_accel  else f'{args.accel_hz} Hz, {args.samples} samples/packet'}")
    print(f"  Inclin  : {'OFF' if args.no_inclin else f'{args.slow_hz} Hz'}")
    print(f"  Temp    : {'OFF' if args.no_temp   else f'{args.slow_hz} Hz'}")
    print(f"  Duration: {'forever' if args.duration == 0 else f'{args.duration}s'}")
    print()

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_connect    = on_connect
    client.on_disconnect = on_disconnect
    client.connect(args.broker, args.port, keepalive=60)
    client.loop_start()

    publishers = []
    for node_id in args.nodes:
        pub = NodePublisher(
            node_id          = node_id,
            client           = client,
            accel_hz         = args.accel_hz,
            slow_hz          = args.slow_hz,
            samples_per_burst= args.samples,
            enable_accel     = not args.no_accel,
            enable_inclin    = not args.no_inclin,
            enable_temp      = not args.no_temp,
        )
        pub.start()
        publishers.append(pub)

    try:
        if args.duration > 0:
            time.sleep(args.duration)
        else:
            while True:
                time.sleep(1)
    except KeyboardInterrupt:
        print("\nStopping…")
    finally:
        for pub in publishers:
            pub.stop()
        for pub in publishers:
            pub.join(timeout=2)
        client.loop_stop()
        client.disconnect()
        print("Done.")


if __name__ == "__main__":
    main()