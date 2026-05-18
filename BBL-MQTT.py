#!/usr/bin/env python3
"""
Bambu Lab MQTT monitor
Prints all messages received from the printer to the console.

Requirements:
    pip install paho-mqtt
"""

import ssl
import json
import paho.mqtt.client as mqtt

# ── Config ─────────────────────────────────────────────
PRINTER_IP  = "192.168.0.152"   # your printer IP
ACCESS_CODE = "cbbfa194"       # 8-char code from printer screen

MQTT_PORT   = 8883
MQTT_USER   = "bblp"
# ──────────────────────────────────────────────────────


def on_connect(client, userdata, flags, rc):
    codes = {
        0: "Connected",
        1: "Bad protocol version",
        2: "Client ID rejected",
        3: "Broker unavailable",
        4: "Bad credentials",
        5: "Not authorised",
    }
    print(f"[MQTT] {codes.get(rc, f'Unknown ({rc})')}")
    if rc == 0:
        client.subscribe("#")
        print("[MQTT] Subscribed to #")


def on_message(client, userdata, msg):
    topic = msg.topic

    # Pretty-print if JSON, raw otherwise
    try:
        print(len(msg.payload))
        payload = json.loads(msg.payload.decode())
        pretty  = json.dumps(payload, indent=2)
    except Exception:
        pretty  = msg.payload.decode(errors="replace")

    print(f"\n── {topic} {'─' * max(0, 60 - len(topic))}")
    print(pretty)


def on_disconnect(client, userdata, rc):
    print(f"[MQTT] Disconnected (rc={rc})")


def main():
    client = mqtt.Client()
    client.username_pw_set(MQTT_USER, ACCESS_CODE)

    # TLS – skip cert verification (Bambu uses self-signed)
    tls_ctx = ssl.create_default_context()
    tls_ctx.check_hostname = False
    tls_ctx.verify_mode    = ssl.CERT_NONE
    client.tls_set_context(tls_ctx)

    client.on_connect    = on_connect
    client.on_message    = on_message
    client.on_disconnect = on_disconnect

    print(f"[MQTT] Connecting to {PRINTER_IP}:{MQTT_PORT}…")
    client.connect(PRINTER_IP, MQTT_PORT, keepalive=60)

    try:
        client.loop_forever()
    except KeyboardInterrupt:
        print("\n[MQTT] Stopped.")
        client.disconnect()


if __name__ == "__main__":
    main()