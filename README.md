# BBL-Smart-Heater
ESP8266 based heater control board for a Bambu Lab P1S

An open-source aftermarket chamber heater controller that connects to your Bambu printer's MQTT broker, reads bed setpoint and chamber temperature, and automatically controls a relay (heater) and PWM fan based on configurable thresholds.

---

## Features

- Connects to Bambu Lab printer via MQTT over TLS
- Automatic heater and fan control based on bed setpoint
- Configurable temperature thresholds and fan speeds via web portal
- Chamber over-temperature safety latch with hysteresis
- Data watchdog — opens config portal if printer stops responding
- Magic bed temperature trigger to open config portal remotely
- Settings persisted to EEPROM across reboots

---

## Hardware

| Component | Details |
|---|---|
| Board | [ESP8266 Relay Module](https://www.aliexpress.com/item/1005010435047645.html) |
| Relay | Onboard, controls heater on GPIO4 |
| Fan | 4-wire PWM fan on GPIO0, 25kHz |
| Flash | 1MB minimum |

---

## Libraries

| Library | Version |
|---|---|
| ArduinoJson | 7.4.3 |
| WiFiManager | 2.0.17 |
| PubSubClient | 2.8 |

---

## Configuration

On first boot (or when no valid config is found in EEPROM), the device will start a WiFi access point:

**SSID:** `BambuSmartHeaterV1`

Connect to it and navigate to `192.168.4.1` to configure:

| Field | Description | Default |
|---|---|---|
| Bambu Printer IP | Local IP of your printer | — |
| Access Code | 8-character printer access code | — |
| Heater On Bed Temp (°C) | Bed setpoint at which heater enables | 90 |
| Fan On Bed Temp (°C) | Bed setpoint at which fan enables | 50 |
| Max Fan Speed (%) | PWM duty cycle at full speed | 100 |
| Min Fan Speed (%) | PWM duty cycle at idle | 0 |
| Chamber Max Temp (°C) | Over-temperature safety trip point | 70 |

Your printer's access code can be found in **Bambu Studio → Device → Connection** or on the printer's own network settings screen.

---

## Re-opening the Config Portal

Since the device lives inside the printer, there is no physical button. Two methods are available:

**1. Magic bed temperature**
Set your printer's bed temperature to `1°C` via any means (Bambu Studio, Home Assistant, etc.). The device will detect this, disconnect from MQTT, and open the config portal.

**2. Data watchdog**
If the device does not receive a valid MQTT message within 60 seconds (e.g. printer is offline or unreachable), it will automatically open the config portal.

In both cases the portal times out after 120 seconds and the device reboots.

---

## Control Logic

**Heater** turns on when:
- Bed setpoint ≥ `Heater On Bed Temp`
- Bed setpoint < 120°C (sanity limit)
- Chamber temperature has not exceeded `Chamber Max Temp`

**Fan** turns on when:
- Bed setpoint ≥ `Fan On Bed Temp`
- Bed setpoint < 120°C (sanity limit)

**Chamber over-temperature latch:**
Once the chamber hits `Chamber Max Temp`, the heater is disabled until the chamber cools to `Chamber Max Temp - 10°C`. The fan continues to run independently.

---

## Safety

- Heater is forced off on boot until a valid MQTT message is received
- Hardware watchdog reboots the device if no data is received within 60 seconds
- Chamber over-temperature latch prevents runaway heating
- Bed setpoint sanity check prevents operation above 120°C

---

## License

MIT