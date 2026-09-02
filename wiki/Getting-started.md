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
| `TX485_Rx` / `TX485_Tx` | **Atomic base on Atom Lite:** omit in `config.h` (env **22/19**). **Tail485 / Unit RS485 on Atom Lite:** uncomment **`#undef` / 32/26 block** in `config-example.h`. **Generic ESP32 dev board:** set pins here (e.g. **16/17**) |
| `AUTO_TX` | **`true`** for auto-direction transceivers (required). Separate DE/RE GPIO is **not supported**. |

**Which env and pins for Atom Lite?**

| RS485 module | How it attaches | PlatformIO env | Pins |
|--------------|-----------------|----------------|------|
| **Atomic RS485 Base** | Tail stack | `M5AtomLite-tub` | **22 / 19** — omit overrides in `config.h` |
| **Tail485** | Tail stack (**not** Grove) | `M5AtomLite-tub` | **32 / 26** — enable **`#undef` block** in `config.h` |
| **Unit RS485** | Grove cable | `M5AtomLite-tub` | **32 / 26** — enable **`#undef` block** in `config.h` |

See [Hardware targets — alternate 32/26](Hardware-targets#atom-lite--alternate-rs485-3226-pins). **Generic `ESP32ota`** is for **non-M5 ESP32 dev boards** only (`board = esp32dev`).

Do not use **GPIO 16/17** on Atom Lite (PICO flash). The **`#undef` block** overrides env `-D` pins; **`#ifndef`-only** lines do not.

Also copy local upload/monitor ports (gitignored):

```bash
cp platformio_local.ini.example platformio_local.ini
```

Edit `upload_port` / `monitor_port` per env (USB `/dev/cu.…` or OTA `spa-XXXXXXXXXXXX.local` / LAN IP). Or pass `--upload-port` on the CLI.

### 3. Build and flash firmware

**M5 Atom Lite** (Atomic base, Tail485, or Unit RS485 — all use `M5AtomLite-tub` for first USB flash):

```bash
# Atomic RS485 Base (22/19): default config.h — no pin overrides
pio run -e M5AtomLite-tub -t upload

# Tail485 or Unit RS485 (32/26): uncomment the #undef / 32/26 block in config.h first
pio run -e M5AtomLite-tub -t upload
```

**Generic ESP32 dev board** (not Atom Lite — `ESP32ota`, pins in `config.h`; OTA is default upload protocol):

```bash
pio run -e ESP32ota -t upload --upload-protocol esptool
```

Set `upload_port` / `monitor_port` in **`platformio_local.ini`** (copy from [`platformio_local.ini.example`](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/platformio_local.ini.example)) or pass `--upload-port /dev/cu.…` if PlatformIO does not auto-detect USB.

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
