import paho.mqtt.client as mqtt

import json
 
latest = {}
 
def on_message(client, userdata, msg):

    topic = msg.topic

    data = json.loads(msg.payload)

    node_id = topic.split("/")[1]

    # Get first accel sample timestamp (in µs)

    first_ts = data["a"][0][0]

    latest[node_id] = first_ts
 
    # Only compare when we have one packet from each node

    if len(latest) == 2:

        ids = list(latest.keys())

        delta_us = abs(latest[ids[0]] - latest[ids[1]])

        delta_ms = delta_us / 1000

        print(f"[{ids[0]}] ts={latest[ids[0]]}  [{ids[1]}] ts={latest[ids[1]]}  delta={delta_ms:.3f} ms")
 
client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)

client.on_message = on_message

client.connect("localhost", 1883)

client.subscribe("wind_turbine/+/data")

client.loop_forever()
 