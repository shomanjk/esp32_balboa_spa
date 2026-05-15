# Getting started

Checklist for a first **tub-side** install (M5 Atom Lite + Atomic RS485 Base). For generic ESP32 boards, use the same steps but set UART pins per the [README M5 / pin section](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/README.md#m5-atom-lite--atomic-rs485-base-tub-side) and [`src/config-example.h`](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/src/config-example.h).

---

## Before you wire

1. Read [ccutrer physical layer notes](https://github.com/ccutrer/balboa_worldwide_app/wiki#physical-layer) for **where to tap RS485** on your controller (A/B, ground reference, accessory power).
2. **12 V / GND** on the M5 Atomic base are **not** the RS485 A/B pair — power the Atom per M5 docs; connect only the bus wires to the spa link.
3. Bench testing with **USB 5 V** only is fine; verify frames before closing the spa cabinet.

---

## Software checklist

### 1. Clone and submodules

```bash
git clone https://github.com/shomanjk/esp32_balboa_spa.git
cd esp32_balboa_spa
git submodule update --init --recursive
```

The LittleFS web UI build requires the **`balboa-spa`** submodule. If `uploadfs` fails with submodule errors, re-run the command above.

### 2. Configure `src/config.h`

```bash
cp src/config-example.h src/config.h
```

Edit at minimum:

| Setting | Notes |
|---------|--------|
| `WIFI_SSID` / `WIFI_PASSWORD` | **2.4 GHz** Wi‑Fi (ESP32 has no 5 GHz) |
| `MQTT_SERVER` / `MQTT_PORT` | Optional until you add HA; required for MQTT/HA |
| `BROKER_LOGIN` / `BROKER_PASS` | If your broker requires auth |
| `TX485_Rx` / `TX485_Tx` | **22 / 19** for Atom on Atomic RS485 Base |
| `AUTO_TX` | Prefer **`true`** unless your transceiver needs explicit DE/RE |

Comment out or remove the default **GPIO 16/17** pair if you use the M5 stack pins.

### 3. Build and flash firmware

```bash
pio run -e M5AtomLite-tub -t upload
```

Set `upload_port` in [`platformio.ini`](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/platformio.ini) or pass `--upload-port /dev/cu.…` if PlatformIO does not auto-detect USB.

### 4. Flash filesystem (web UI)

```bash
pio run -e M5AtomLite-tub -t uploadfs
```

Run after the first firmware upload, and again when the web bundle changes.

### 5. Verify on the LAN

1. Find the device: serial log shows hostname/IP, or try `http://spa-XXXXXXXXXXXX.local/` (hostname is derived from Wi‑Fi MAC — see [`wifiModule.cpp`](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/lib/wifiModule/wifiModule.cpp)).
2. Open **`/status`** — live water temp and equipment should update within a short time.
3. Open **`/state`** — Wi‑Fi, firmware version, RS485 health summary.
4. Optional JSON checks:
   - `GET /api/version`
   - `GET /api/rs485` — counters and `health`
   - `GET /api/status/controls` — snapshot used by `/status` polling

**RGB LED (M5 `M5AtomLite-tub`):** green = Wi‑Fi has IP; red = disconnected; blue/yellow flashes = coarse RS485 activity.

### 6. Optional: MQTT and Home Assistant

Follow [Home Assistant setup](Home-Assistant-setup) once the spa bus is healthy.

---

## Hardware quick reference (M5 stack)

| Item | Value |
|------|--------|
| PlatformIO env | `M5AtomLite-tub` |
| UART2 RX / TX | GPIO **22** / **19** |
| `AUTO_TX` | **`true`** recommended |
| No frames? | Verify A/B (swap if needed); check `/api/rs485` |

Full detail: [README — M5 Atom Lite + Atomic RS485 Base](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/README.md#m5-atom-lite--atomic-rs485-base-tub-side).

---

## Next steps

- [Troubleshooting FAQ](Troubleshooting-FAQ) if `/status` stays empty
- [Home Assistant setup](Home-Assistant-setup)
- [OTA workflow](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/OTA_LOGGING_WORKFLOW.md) for wireless updates

[← Wiki home](Home)
