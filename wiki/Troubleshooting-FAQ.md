# Troubleshooting FAQ

Short **symptom → checks** guide. For release-accurate API and flag lists, use the [README](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/README.md).

---

## No spa data / empty `/status`

**Checks (in order):**

1. **Wiring** — Confirm RS485 **A/B** on the spa bus ([ccutrer physical layer](https://github.com/ccutrer/balboa_worldwide_app/wiki#physical-layer)). Swap A/B if you see no frames.
2. **Pins** — `M5AtomLite-tub`: env **RX 22 / TX 19**. `M5AtomS3Lite-tub` (AtomS3 Lite): env **RX 5 / TX 6**. Generic envs: set pins in `config.h`. On Atom Lite, **GPIO 16/17** are unsafe (PICO flash) — never use them; immediate safe mode on 16/17 applies only on **`M5AtomLite-tub`** (pin guard needs `M5_STATUS_LED`).
3. **`AUTO_TX`** — Use **`true`** with auto-direction transceivers. Separate DE/RE GPIO control is **not supported**.
4. **RS485 health** — Open `/state` or `GET /api/rs485`. Look at `health`, frame/CRC counters, and polarity hints. `UART_DEFERRED` means waiting for Wi‑Fi/OTA; `RS485_SAFE_MODE` means UART is skipped after faults — fix pins then `POST /api/rs485/retry` or power-cycle.
5. **Power / bench** — USB-only bench power is OK; ensure the transceiver shares a valid ground reference with the bus.

**Deeper (maintainer checklist in repo):**

- [RS485 5-minute diagnostic checklist](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/docs/rs485-5min-diagnostic-checklist.md)

---

## Portal hangs, 503, or unstyled pages

Symptoms: `/status` or other portal pages freeze, show raw CSS as text, return **503** (`portal page busy` or low-memory retry), or the device drops off the LAN mid-page load.

**Checks (in order):**

1. **Firmware version** — Upgrade to **[v2.28.0](https://github.com/shomanjk/esp32_balboa_spa/releases/tag/v2.28.0)** or newer. On **ESP32-PICO-D4** (M5 Atom Lite), releases before **2.28** could exhaust DRAM assembling large portal pages. Overview: [Discussions #33](https://github.com/shomanjk/esp32_balboa_spa/discussions/33).
2. **`GET /api/version`** — Confirm the running version matches what you expect after OTA.
3. **Auto-refresh page** — A tiny “retry in 2 seconds” page means low-memory guard triggered; wait for the refresh or close extra browser tabs hitting the gateway at once.
4. **503 `portal page busy`** — Only one large portal page assembles at a time; retry after the other request finishes.
5. **Still broken on 2.28+?** — Open an Issue with firmware version, browser, and whether RS485 was connected; optional serial excerpt.

Atom Lite users on **alternate RS485 at 32/26** (Tail485 tail or Unit RS485 Grove — not Atomic base): also see [Wrong pins on Atom Lite](#wrong-pins-on-atom-lite) below — portal symptoms can overlap with RS485 retry floods on older firmware.

---

## Wrong pins on Atom Lite

Symptom: no spa frames despite “correct” wiring, or RS485 safe mode with GPIO **16/17**.

**Cause:** [M5 Atom Lite](https://docs.m5stack.com/en/core/ATOM%20Lite) is **ESP32-PICO-D4**. Three RS485 options, two pin maps:

| RS485 module | How it attaches | Env | Pins in `config.h` |
|--------------|-----------------|-----|-------------------|
| **Atomic RS485 Base** | Tail stack | `M5AtomLite-tub` | Omit overrides → **22 / 19** (env default) |
| **Tail485** | Tail stack (**not** Grove) | `M5AtomLite-tub` | **`#undef` block** → **32 / 26** |
| **Unit RS485** | Grove cable | `M5AtomLite-tub` | **`#undef` block** → **32 / 26** |

**Common mistake:** Tail485 or Unit RS485 wired to **32/26** but `config.h` still leaves env defaults **22/19** (no `#undef` block) — UART talks to the wrong GPIOs.

**Also:** **GPIO 16/17** are PICO flash — never use on Atom Lite. On **`M5AtomLite-tub`**, 16/17 trigger immediate safe mode. On **`ESP32ota`**, the pin guard is off.

Detail: [Hardware targets — alternate RS485 (32/26 pins)](Hardware-targets#atom-lite--alternate-rs485-3226-pins).

---

## Wi‑Fi won’t connect

1. **`config.h`** — Correct `WIFI_SSID` and `WIFI_PASSWORD` (rebuild after edits).
2. **Band** — Use a **2.4 GHz** SSID; ESP32 does not join 5 GHz-only networks.
3. **`/state`** — Shows Wi‑Fi status, RSSI, and IP when connected.
4. **`GET /api/wifi`** — JSON mirror of station state (see [README JSON APIs](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/README.md#json-api-get)).

If the device stays offline long enough, firmware may self-restart (see `WIFI_OFFLINE_RESTART_*` comments in [`config-example.h`](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/src/config-example.h)).

---

## MQTT connected but no Home Assistant entities

1. **Same broker** — Home Assistant’s [MQTT integration](https://www.home-assistant.io/integrations/mqtt/) must use the **same broker** as `MQTT_SERVER` in `config.h`.
2. **Discovery enabled** — Default is on; to disable: `#define MQTT_HA_DISCOVERY 0` in `config.h`.
3. **Discovery prefix** — Default `homeassistant/`; override with `MQTT_DISCOVERY_PREFIX` if your HA uses a custom prefix.
4. **Temperature unit** — Default discovery uses **°F**. Celsius tubs: set `MQTT_HA_TEMP_UNIT` in `config.h` (see [config-example.h](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/src/config-example.h)).
5. **Wait for spa config** — Core entities appear after MQTT connect; **equipment** switches/selects may appear only after the spa sends **config/information** frames (same as web `/status`).

Canonical topic and entity list: [README — MQTT and Home Assistant](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/README.md#mqtt-and-home-assistant).

Walkthrough: [Home Assistant setup](Home-Assistant-setup).

---

## Command “accepted” but the tub does not change

1. **One action at a time** — Avoid overlapping web, MQTT, and bridge tests.
2. **Confirm RS485 is healthy** — See [No spa data](#no-spa-data--empty-status) above.
3. **Web vs LittleFS SPA** — Firmware **`/status`** controls are wired to RS485; the bundled **`balboa-spa`** tab may differ — prefer `/status` when debugging writes.
4. **Bridge harness cooldown** — If using TCP port **4257**, wait between raw frame injections.

**Deeper:**

- [Command-write debug log](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/docs/command-write-debug-log.md) (maintainer ledger)

---

## OTA update fails

1. **LAN** — Computer and spa gateway on the same network; know hostname (`spa-….local`) or IP.
2. **Env** — Use `M5AtomLite-tub-ota` with `upload_protocol = espota`.
3. **Auth** — Shipped builds often have OTA auth **off**; if you enabled `ENABLE_OTA_AUTH`, set `OTA_PASSWORD` in `config.h`.
4. **Recovery** — USB flash with `M5AtomLite-tub` if OTA is unreachable.

Full runbook: [OTA_LOGGING_WORKFLOW.md](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/OTA_LOGGING_WORKFLOW.md).

---

## mDNS / `.local` hostname does not resolve

The gateway hostname is `spa-` + MAC-derived suffix (e.g. `spa-aabbccddeeff.local`). Some networks block mDNS.

- Use the IP from `/state`, serial log, or your router’s DHCP table.
- For HA `configuration_url`, use a DHCP reservation and local DNS if needed (see [README](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/README.md#mqtt-and-home-assistant)).

---

## Still stuck?

Open a [GitHub Issue](https://github.com/shomanjk/esp32_balboa_spa/issues) with:

- Board / env (e.g. `M5AtomLite-tub`)
- Firmware version from `/state` or `/api/version`
- Symptom, `/api/rs485` snapshot (redact Wi‑Fi/MQTT secrets)
- Optional: add a row on [Hardware field notes](Hardware-field-notes)

[← Wiki home](Home)
