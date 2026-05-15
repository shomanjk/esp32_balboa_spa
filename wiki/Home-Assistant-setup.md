# Home Assistant setup

Practical walkthrough for MQTT telemetry and **MQTT Discovery**. Command topic reference remains in the [README](https://github.com/shomanjk/esp32_balboa_spa/blob/main/README.md#mqtt-and-home-assistant).

---

## Prerequisites

1. Gateway flashed and **`/status`** shows live spa data ([Getting started](Getting-started)).
2. An MQTT broker reachable from both the ESP32 and Home Assistant (often the Mosquitto add-on on the same HA host).
3. `config.h` broker settings: `MQTT_SERVER`, `MQTT_PORT`, and credentials if required.

---

## 1. Gateway name and topic prefix

On Wi‑Fi connect, firmware sets hostname and MQTT client id to:

`spa-` + last 12 hex digits of the Wi‑Fi MAC (example: `spa-a1b2c3d4e5f6`).

MQTT topics use:

```text
Spa/<gatewayName>/…
```

Examples:

- `Spa/spa-a1b2c3d4e5f6/status/…` (telemetry — see repo for full tree)
- `Spa/spa-a1b2c3d4e5f6/cmd/…` (commands)
- `Spa/spa-a1b2c3d4e5f6/node/state` (availability)

Find your exact name on **`/state`**, serial log, or `GET /api/version` (`hostname` field).

---

## 2. Configure Home Assistant MQTT

1. Install/configure the [MQTT integration](https://www.home-assistant.io/integrations/mqtt/) pointing at the **same broker** as the ESP32.
2. After the gateway connects, open **Settings → Devices & services → MQTT** and look for device **Balboa Spa**.

Discovery publishes retained configs under:

```text
homeassistant/<platform>/<object_id>/config
```

Default prefix is `homeassistant/`. Override in `config.h`:

```c
#define MQTT_DISCOVERY_PREFIX "homeassistant"
```

To disable discovery entirely:

```c
#define MQTT_HA_DISCOVERY 0
```

---

## 3. Celsius tubs

Discovery uses a **static** `unit_of_measurement` (default **°F**). If your tub reports °C, add to `config.h` (see [config-example.h](https://github.com/shomanjk/esp32_balboa_spa/blob/main/src/config-example.h)):

```c
#define MQTT_HA_TEMP_UNIT "\xC2\xB0" "C"
```

Rebuild and flash after changing `config.h`.

---

## 4. What appears when

| Phase | Typical entities |
|-------|------------------|
| Right after MQTT connect | Core temps, climate (`spa_controls`), many status sensors, availability |
| After spa **config / information** frames | Equipment switches (lights, blower, mister), pump **switch** or **select** per speed, extra enum sensors |

If pumps/lights are missing, wait for the spa to finish its config exchange, refresh MQTT devices, or check `/status` on the gateway web UI.

**Not discovered:** `spa_time` (panel clock) — intentionally omitted to avoid noisy history.

**Device link:** Discovery sets `configuration_url` to `http://<gatewayName>.local/status`. If mDNS fails, open the gateway by IP instead.

---

## 5. Example: set temperature via automation

Replace `spa-a1b2c3d4e5f6` with your gateway name.

**Developer tools → Actions → MQTT publish:**

| Field | Value |
|-------|--------|
| Topic | `Spa/spa-a1b2c3d4e5f6/cmd/setTemp` |
| Payload | `102` (°F example) or `39.5` (°C example) |

**YAML automation snippet:**

```yaml
action: mqtt.publish
data:
  topic: Spa/spa-a1b2c3d4e5f6/cmd/setTemp
  payload: "102"
```

Check result JSON on `Spa/<gateway>/cmd/result` (`accepted`, `reason`).

---

## 6. Example: toggle a load via button command

Non-pump buttons accept `on`, `off`, or `toggle`:

```text
Topic: Spa/<gateway>/cmd/button/<code>
Payload: toggle
```

Pump buttons use `Off`, `Low`, `High` (single-speed pumps: `Off` / `Low` only). Button codes match your spa config — use `/status` or MQTT status topics to identify equipment.

Full v1 command list: [README — MQTT command topics](https://github.com/shomanjk/esp32_balboa_spa/blob/main/README.md#mqtt-command-topics-v1).

---

## 7. Troubleshooting HA

| Issue | Fix |
|-------|-----|
| No MQTT device | Same broker as ESP; gateway online; check `Spa/<gateway>/node/state` |
| Wrong temperature unit | Set `MQTT_HA_TEMP_UNIT` and reflash |
| Missing pumps/lights | Wait for config frames; verify `/status` shows equipment |
| Link from HA device page 404 | mDNS — use IP or fix local DNS |

More symptoms: [Troubleshooting FAQ](Troubleshooting-FAQ).

---

[← Wiki home](Home) · [Getting started](Getting-started)
