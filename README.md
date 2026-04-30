# esp32_balboa_spa

## About this fork

**Lineage:** Firmware and documentation build on the ESP32 port **[NorthernMan54/esp32_balboa_spa](https://github.com/NorthernMan54/esp32_balboa_spa)** (branch **`ESP32`**). Ongoing changes are tracked from **[shomanjk/esp32_balboa_spa](https://github.com/shomanjk/esp32_balboa_spa)** (`origin` for typical clones). The ESP8266-era appendix in the [collapsed snapshot](#inherited-readme-snapshot) comes from [cribskip/esp8266_spa](https://github.com/cribskip/esp8266_spa) via NorthernMan54’s README; those older repos are **not** the target for day-to-day pull requests from this line.

**Maintained here (this fork):**

- **[M5 Atom Lite + Atomic RS485 Base](#m5-atom-lite--atomic-rs485-base-tub-side)** tub-side wiring, **`M5_ATOM_LED`**, and related **PlatformIO** environments in [`platformio.ini`](platformio.ini).
- **RS485** robustness, diagnostics, and **JSON APIs** (`/api/rs485`, `/api/rs485/raw`, `/api/rs485/history`) — see [CHANGELOG.md](CHANGELOG.md).
- **Web** portal for `/status`, `/config`, `/state` (responsive layout, Wi‑Fi chart, **live spa + heating polling**, **RS485 command controls** on `/status`, firmware version on `/state`) and **`GET /api/version`**, **`/api/wifi`**, **`/api/status/controls`**, etc.
- **MQTT** telemetry on `Spa/<gateway>/…` topics and **Home Assistant MQTT Discovery** (retained `homeassistant/…/config`); optional `config.h` overrides — see [MQTT and Home Assistant](#mqtt-and-home-assistant).
- **OTA** workflow for tub-side Atom builds.

**Also inherited in code (may still matter to you):**

- **`BRIDGE`** / TCP port **4257** for LAN clients such as [homebridge-plugin-bwaspa](https://github.com/vincedarley/homebridge-plugin-bwaspa) — summarized under [Optional: Homebridge and TCP bridge](#optional-homebridge-and-tcp-bridge-bridge).
- **Remote + ePaper** build profile (`REMOTE_CLIENT`, `spaEpaper`) — see [ePaper remote display](#epaper-remote-display-optional).

**Pointers:** [CHANGELOG.md](CHANGELOG.md) · [GitHub Releases](https://github.com/shomanjk/esp32_balboa_spa/releases) · [FORK.md](FORK.md) · [AGENTS.md](AGENTS.md).

---

## What this project is

Use an **ESP32** module on the **RS485** bus of a **Balboa** spa controller to read status (temperatures, pumps, configuration, etc.), **send v1 spa commands** from the firmware **web portal** (`/status` controls and `/devices/sci`), and expose state over **MQTT**, the bundled **web UI** (LittleFS SPA), and optionally a **remote ePaper** UI.

The codebase supports multiple **build roles**: tub-side **RS485** gateway, **UDP discovery** (`LOCAL_CONNECT`), optional **TCP bridge**, **Telnet** logging, and a **TCP client + ePaper** role for a second device (e.g. kitchen display) talking to the tub-side unit.

---

## Features

- Caching of hot tub configuration to reduce traffic to the spa controller and improve client responsiveness.
- Hourly tracking of hot tub temperature for 24 hours.
- Daily tracking of heater on-time (seconds), 24 days of history.
- Daily tracking of filter on-time (seconds), 24 days of history.

---

## ePaper remote display (optional)

![ePaper display example](docs/ePaper-Sept2024.jpeg)

Display builds use the [LilyGo T5 ePaper](https://www.lilygo.cc/en-ca/products/t5-4-7-inch-e-paper-v2-3?srsltid=AfmBOopva5B_jxFAsa86Fn75lR66ZpcsqNLJEqPG4Axu8zeuCEEeqI0D). **Note:** upstream ePaper development has stalled because a reference device [stopped working](https://github.com/Xinyuan-LilyGO/LilyGo-EPD47/issues/137); treat this path as best-effort.

---

## Web interface

Credit for the SPA web app: [jozefnad/balboa-spa](https://github.com/jozefnad/balboa-spa) (this repo pins a fork as a submodule — see [Build with PlatformIO](#build-with-platformio)).

![Web UI example](docs/balboa-spa-web.png)

**Status:** Firmware-served **`/status`** includes **wired** equipment, temperature, and panel-clock controls (SCI **`Button`** / **`SetTemp`** / **`SystemTime`** to the spa over RS485). The **LittleFS** `balboa-spa` bundle may still differ from upstream; treat its control surfaces as **read-first** unless you verify them for your build.

**Layout:** Firmware-served pages `/status`, `/config`, and `/state` use a responsive layout (viewport scaling, wrapping nav, fluid charts/images) for phone-sized screens.

### Screenshots (optional assets)

Place images under [`docs/`](docs/) or update paths below.

#### Spa status (`/status`)

![Spa status page](docs/spa-status.png)

#### Spa configuration (`/config`)

![Spa configuration page](docs/spa-config.png)

#### ESP state (`/state`)

![ESP state page](docs/esp-state.png)

- Operator-first layout: Wi-Fi and system/RS485 health are surfaced at the top.
- `Show advanced diagnostics` reveals low-level counters and time-debug internals on demand.
- Advanced mode includes `API Shortcuts` links for `/api/wifi`, `/api/version`, `/api/rs485`, `/api/rs485/raw`, and `/api/rs485/history`.

### JSON API (`GET`)

| Path | Query | Purpose |
| --- | --- | --- |
| `/api/version` | — | Firmware `version`, `build`, `hostname`, `ip`, `restartReason`. |
| `/api/wifi` | — | Wi‑Fi station: `connected`, `status` / `statusName`, `mac`, `hostname`; when connected: `ssid`, `rssi`, `ip`, `gateway`, `subnet`, `dns`, `channel`. |
| `/api/rs485` | — | UART pins, baud, `autoTx`, byte/frame/CRC counters, polarity (`normal` / `inverted_rx_tx`), lock state, `health`. |
| `/api/rs485/raw` | `limit` (default **80**, cap **256**) | Bounded recent RX bytes: `bytesHex`, `items[]` with `tMs`, `gapMs`, `byte`, `mode`, `uartAvailable`. |
| `/api/rs485/history` | `limit` (default **20**, cap **60**) | Rolling RS485 snapshots (newest first): per-snapshot `health`, counters, `mode`, `detectPhase`. |
| `/api/status/controls` | — | Live snapshot for **`/status`** polling: pumps, lights, temps, **spa/heating** fields, setpoint bounds, and snapshot freshness metadata. |

---

## MQTT and Home Assistant

**Telemetry:** With Wi‑Fi and broker settings in `config.h` (`MQTT_SERVER`, `MQTT_PORT`, credentials), the gateway publishes spa state under `Spa/<gateway>/…` (see [AGENTS.md](AGENTS.md)).

**MQTT commands:** The gateway subscribes `Spa/<gateway>/cmd/#` and dispatches commands to the shared spa dispatcher. Command outcomes are published as JSON on `Spa/<gateway>/cmd/result`.

### Command write scope (v1)

To keep protocol risk low, command-write implementation is intentionally staged:

- **In scope (v1) — implemented on web + MQTT paths:**
  - **Button toggle** commands (Balboa `0x11`) from **`/devices/sci`** and firmware **`/status`** (including temp-range item **80** / `0x50`).
  - **Set temperature** commands (Balboa `0x20`) with **protocol** min/max by °F/°C and active high/low range ([`spaProtocolActiveSetpointBand`](lib/spaMessage/spaCommandDispatcher.cpp)).
- **Also in scope (v1):**
  - **Set panel clock** (`SystemTime`, Balboa `0x21`) via `HH:MM` or gateway sync.
- **Deferred (post-v1):**
  - `TimeFormat`
  - `TempUnits`

### MQTT command topics (v1)

- `Spa/<gateway>/cmd/setTemp` -> numeric payload (`102`, `39.5`)
- `Spa/<gateway>/cmd/setTime` -> `HH:MM`
- `Spa/<gateway>/cmd/syncTime` -> any non-empty payload (uses gateway local time)
- `Spa/<gateway>/cmd/mode` -> `heat` or `off` (Ready/Rest)
- `Spa/<gateway>/cmd/preset` -> `Low Range` or `High Range`
- `Spa/<gateway>/cmd/button/<code>` ->
  - non-pump: `on`, `off`, `toggle`
  - pumps: `Off`, `Low`, `High` (single-speed pumps accept `Off`/`Low`)
- Result telemetry: `Spa/<gateway>/cmd/result` JSON (`target`, `value`, `accepted`, `reason`)

All command frame semantics should be validated against the ccutrer protocol reference before enabling each command family:
- [ccutrer/balboa_worldwide_app `doc/protocol.md`](https://github.com/ccutrer/balboa_worldwide_app/blob/main/doc/protocol.md)

To avoid repeating dead-end experiments while command-write behavior is being debugged, keep the running attempt ledger up to date:
- [`docs/command-write-debug-log.md`](docs/command-write-debug-log.md)

**Home Assistant MQTT Discovery:** After each successful MQTT connect, the firmware publishes **retained** discovery configs under `homeassistant/<platform>/<object_id>/config`. In Home Assistant, entities appear under the MQTT integration as device **Balboa Spa**.

- **Temperature values:** `current_temp`, `set_temp`, `low_set_temp`, `high_set_temp`, `sensor_a`, `sensor_b` are numeric temperature sensors.
- **Writable controls:** `climate` (`spa_controls`), load `switch` entities (`light1`, `light2`, `blower`, `mister`), pump controls by capability (**1-speed pumps as `switch`**, **2-speed pumps as `select`**), a `button` for panel-time sync, and diagnostic `sensor` for last command result.
- **Enum/categorical values:** `heating_state`, `spa_state`, `init_mode`, `heating_mode`, `filter_mode`, `temp_range` publish human-readable strings and are discovered as enum sensors (not numeric measurements).
- **Binary values:** `panel_locked`, `settings_lock`, `circ`, `blower`, `light1`, `light2`, `mister` are binary sensors with explicit on/off payloads.
- **Clock entity:** `spa_time` is intentionally **not** discovered in HA to avoid noisy, non-actionable history churn.
- **Device web link:** Discovery sets `device.configuration_url` to `http://<gatewayName>.local/status` so the HA device page can open the ESP web status page directly.

- **Broker:** Use the Home Assistant [MQTT integration](https://www.home-assistant.io/integrations/mqtt/) on the **same broker** as the ESP32. Default discovery prefix is `homeassistant/` (override with `MQTT_DISCOVERY_PREFIX` in `config.h` / defaults in [`lib/mqttModule/mqttModule.h`](lib/mqttModule/mqttModule.h)).
- **Temperature unit:** Discovery uses a static `unit_of_measurement` (default **°F**). For Celsius tubs, set `MQTT_HA_TEMP_UNIT` in `config.h` (see [`src/config-example.h`](src/config-example.h)).
- **Web-link hostname:** `configuration_url` uses mDNS (`<gatewayName>.local`). If your network does not resolve mDNS, use a DHCP reservation + local DNS, or open the device by IP from HA.
- **Disable discovery:** `#define MQTT_HA_DISCOVERY 0` in `config.h`.

---

## Optional: Homebridge and TCP bridge (`BRIDGE`)

When **`BRIDGE`** is enabled at build time, the firmware exposes a **TCP server on port 4257** that can pair with the Homebridge plugin **[homebridge-plugin-bwaspa](https://github.com/vincedarley/homebridge-plugin-bwaspa)**. This path is **legacy/community** relative to the MQTT + HA focus of this fork; it remains in the codebase for compatibility. See compiler flags below.

### Bridge-first raw troubleshooting harness

For short-lived command-write troubleshooting, you can inject raw Balboa frames over the bridge without reflashing:

- Harness script: [`scripts/bridge_raw_tester.py`](scripts/bridge_raw_tester.py)
- Example matrix input: [`docs/bridge-raw-command-matrix.example.json`](docs/bridge-raw-command-matrix.example.json)
- Troubleshooting ledger: [`docs/command-write-debug-log.md`](docs/command-write-debug-log.md)

Single-case run:

```
python3 scripts/bridge_raw_tester.py \
  --host <spa-ip-or-hostname> \
  --frame-hex "7e070abf110400137e" \
  --label "toggle_pump1_item4"
```

Matrix run:

```
python3 scripts/bridge_raw_tester.py \
  --host <spa-ip-or-hostname> \
  --matrix docs/bridge-raw-command-matrix.example.json \
  --out docs/bridge-raw-last-run.json
```

Guardrails:

- Use one command sequence at a time and keep cooldowns between retries.
- Record expected vs observed behavior for each run in the debug ledger.
- If repeated runs are mostly malformed-frame/operator errors, consider phase 2: add a thin typed HTTP helper endpoint.

---

## Build with PlatformIO

Builds use **PlatformIO** ([`platformio.ini`](platformio.ini)).

### Web UI submodule

The SPA web UI lives in the **`balboa-spa`** git submodule:

- Fork remote: `https://github.com/shomanjk/balboa-spa.git`
- Upstream for merging SPA changes: `https://github.com/jozefnad/balboa-spa.git`

After clone or fresh checkout:

```
git submodule update --init --recursive
```

The LittleFS pre-build script [`scripts/extra_script.py`](scripts/extra_script.py) **requires** an initialized submodule and fails fast with guidance if it is missing.

---

## M5 Atom Lite + Atomic RS485 Base (tub-side)

This fork documents **tub-side** builds on the [M5 Atom Lite](https://docs.m5stack.com/en/core/ATOM%20Lite) stacked on the [Atomic RS485 Base](https://docs.m5stack.com/en/atom/Atomic%20RS485%20Base) (TTL ↔ RS‑485, **SP3485EE**, built-in **12 V→5 V** DC‑DC for the Atom). Generic ESP32 dev boards remain supported; see [`src/config-example.h`](src/config-example.h) for default GPIO **16/17**.

- **PlatformIO environment:** `M5AtomLite-tub` in [`platformio.ini`](platformio.ini) uses `board = m5stack-atom` and partition table `spa_module.csv` (4MB class builds).
- **Pins (Atom on Atomic RS485 Base):** UART2 **RX = GPIO 22**, **TX = GPIO 19** (see M5 [Atomic RS485 Arduino example](https://github.com/m5stack/M5-ProductExampleCodes/blob/master/AtomBase/AtomicRS485/Arduino/AtomicRS485/AtomicRS485.ino)). In `config.h` set **`TX485_Rx 22`** and **`TX485_Tx 19`**. If you see no frames, verify **A/B** on the spa bus (swap if needed); optional **120 Ω** termination between A and B for long runs per M5 guidance.
- **`AUTO_TX`:** Prefer **`AUTO_TX true`** in [`src/config.h`](src/config-example.h) unless your transceiver needs explicit DE/RE.
- **Power:** The base can step **12 V** to **5 V** for the Atom; follow M5 **VH‑3.96** pinout and your controller’s accessory supply. **12 V/GND** are not the same as RS‑485 **A/B** — use the Balboa [physical layer](https://github.com/ccutrer/balboa_worldwide_app/wiki#physical-layer) notes. Bench: **USB 5 V** is fine.
- **RGB LED (`M5AtomLite-tub` only):** With **`M5_ATOM_LED`** in [`platformio.ini`](platformio.ini): **green** = Wi‑Fi has IP; **red** = disconnected; **blue/yellow** flashes bracket RS485 loop work (coarse activity, not per-byte TX/RX) — see [`src/led_control.cpp`](src/led_control.cpp).

Copy [`src/config-example.h`](src/config-example.h) to `src/config.h`, set Wi‑Fi / MQTT / pins, then enable the M5 UART block (and comment out default **16/17** if unused) before `pio run -e M5AtomLite-tub`.

**Alternate wiring:** Generic ESP32 **16/17**, or [M5 Unit RS485](https://docs.m5stack.com/en/unit/rs485) on the Atom Grove (**TX485_Rx 32** / **TX485_Tx 26**) — comments in [`src/config-example.h`](src/config-example.h).

---

## OTA updates (M5 Atom tub-side)

- **ArduinoOTA** is enabled from Wi‑Fi connect ([`lib/wifiModule/wifiModule.cpp`](lib/wifiModule/wifiModule.cpp)); serial logs show hostname/IP.
- Use environment **`M5AtomLite-tub-ota`** (`upload_protocol = espota`). Set `upload_port` in [`platformio.ini`](platformio.ini) to `spa-XXXXXXXXXXXX.local` or the device IP.

```
pio run -e M5AtomLite-tub-ota -t upload
```

- **Auth:** Off by default; set `ENABLE_OTA_AUTH` + `OTA_PASSWORD` in `config.h` for stricter LANs.
- **Recovery:** Keep USB flashing via **`M5AtomLite-tub`** if OTA fails.
- **Visibility:** `/state` shows firmware version/build; **`GET /api/version`** returns JSON for dashboards.
- **Runbook:** [`OTA_LOGGING_WORKFLOW.md`](OTA_LOGGING_WORKFLOW.md).

---

## Compiler definitions

| Flag | Role |
|------|------|
| **`LOCAL_CLIENT`** | RS485 to spa controller (tub-side). |
| **`REMOTE_CLIENT`** | TCP client to tub-side gateway (remote/kitchen device). |
| **`LOCAL_CONNECT`** | UDP discovery on port **30303** (Balboa-style discovery). |
| **`BRIDGE`** | TCP server on **4257** (Homebridge plugin path). |
| **`TELNET_LOG`** | Telnet logging. |
| **`spaEpaper`** | ePaper UI (ESP32-S3 T5 env, etc.). |
| **`M5_ATOM_LED`** | (Optional, **`M5AtomLite-tub`**) Atom RGB status / RS485 activity. |

### Example: spa / tub-side gateway

```
'-DLOCAL_CONNECT'
'-DLOCAL_CLIENT'
'-DBRIDGE'
'-DTELNET_LOG'
```

### Example: remote ePaper module

[LilyGo T5 ePaper](https://www.lilygo.cc/en-ca/products/t5-4-7-inch-e-paper-v2-3?srsltid=AfmBOopva5B_jxFAsa86Fn75lR66ZpcsqNLJEqPG4Axu8zeuCEEeqI0D):

```
'-DREMOTE_CLIENT'
'-DspaEpaper'
```

---

## Background and credits

ESP32 port and modernization of earlier Balboa Wi‑Fi module work. References:

- [cribskip/esp8266_spa](https://github.com/cribskip/esp8266_spa) — ESP8266 reference implementation.
- [EmmanuelLM/esp8266_spa](https://github.com/EmmanuelLM/esp8266_spa) — related ESP8266 material (e.g. wiring diagrams linked from historical READMEs).
- [ccutrer/balboa_worldwide_app](https://github.com/ccutrer/balboa_worldwide_app) — [wiki](https://github.com/ccutrer/balboa_worldwide_app/wiki) and [protocol.md](https://github.com/ccutrer/balboa_worldwide_app/blob/master/doc/protocol.md).

---

<h2 id="inherited-readme-snapshot">Inherited README snapshot (NorthernMan54, branch ESP32)</h2>

The block below is a **frozen copy** of [`README.md` on branch `ESP32`](https://github.com/NorthernMan54/esp32_balboa_spa/blob/ESP32/README.md) as of this documentation pass. It preserves the original **HomeAssistant integration** wording, **Homebridge** first-person note, **compiler definitions**, **cribskip** appendix, OpenHab image, ESP8266 parts list, and **TODO** for historical context. For **accurate** Home Assistant behavior on **this** fork, use [MQTT and Home Assistant](#mqtt-and-home-assistant) above.

<details>
<summary><strong>Show inherited README (NorthernMan54 <code>ESP32</code>)</strong></summary>

# esp32_balboa_spa

WiFI Enable your Balboa SPA using a ESP32 module connected to your spa controller using rs485 interface to Balboa SPA Controller.

Multimode code base, with multiple user interfaces available. Interfaces include MQTT, Web and ePaper display.

Code base can also operate in client mode to a remote implementation of the code base in the Hot Tub. This allows for creating a ePaper display in a central location.

In my setup I have the code base deployed twice, one connected to the Balboa spa controller via rs485. And a second deployment on a [LilyGo T5 ePaper Display](https://www.lilygo.cc/en-ca/products/t5-4-7-inch-e-paper-v2-3?srsltid=AfmBOopva5B_jxFAsa86Fn75lR66ZpcsqNLJEqPG4Axu8zeuCEEeqI0D) that is mounted to the kitchen so you can see the temperature etc.

## Advanced Features

* Caching of hot tub configuration, to reduce number of calls to SPA Controller and improve responsiveness of client applications
* Hourly tracking of hot tub temperature for 24 hours
* Daily tracking of heater on time in seconds. Keeps 24 days of history
* Daily Tracking of filter on time in seconds. Keeps 24 days of history

## This is the ePaper display

![alt text](docs/ePaper-Sept2024.jpeg)

Display is based on the [LilyGo T5 ePaper Display](https://www.lilygo.cc/en-ca/products/t5-4-7-inch-e-paper-v2-3?srsltid=AfmBOopva5B_jxFAsa86Fn75lR66ZpcsqNLJEqPG4Axu8zeuCEEeqI0D)

Pls note I have stopped development of ePaper functionality, as my device [stopped working.](https://github.com/Xinyuan-LilyGO/LilyGo-EPD47/issues/137)

## This is the web site
Credit for the code goes to https://github.com/jozefnad/balboa-spa

![alt text](docs/balboa-spa-web.png)

Currently the WebSite buttons are not working. I never got around to wiring them up.

## Integration with Homebridge

I have used this with the homebridge plugin [homebridge-plugin-bwaspa](https://github.com/vincedarley/homebridge-plugin-bwaspa) to control and automate my Hot Tub.

## MQTT Interface

MQTT Commands are not working yet. I never got around to wiring them up.

# Code Base Build

For the build I use platformio.

## Compiler Definitions

 * LOCAL_CLIENT - Connects to a local SPA via rs485 connection
 * REMOTE_CLIENT - Connects to a remote SPA via TCP / WiFi Module interface 
 * LOCAL_CONNECT - Enable discovery of ESP32 module via the Balboa discovery protocol
 * BRIDGE - Enable local TCP Server - Can be leveraged by https://github.com/vincedarley/homebridge-plugin-bwaspa
 * TELNET_LOG - Enables serial logging via a telnet interface
 * spaEpaper - Enables ePaper display

### In spa configuration

For the unit deployed in the spa, and connected to the Balboa spa controller via rs485 I use these compiler definitions.

```
  '-DLOCAL_CONNECT'
  '-DLOCAL_CLIENT'
  '-DBRIDGE'
  '-DTELNET_LOG'
```

### Remote module configuration

This is for the [LilyGo T5 ePaper Display](https://www.lilygo.cc/en-ca/products/t5-4-7-inch-e-paper-v2-3?srsltid=AfmBOopva5B_jxFAsa86Fn75lR66ZpcsqNLJEqPG4Axu8zeuCEEeqI0D)

```
  '-DREMOTE_CLIENT'
  '-DspaEpaper'
```

## Background / History

This is port of the package to run on an ESP32 Device, and modernization of the package

Based on the great work over at \
https://github.com/cribskip/esp8266_spa
https://github.com/ccutrer/balboa_worldwide_app/wiki
https://github.com/ccutrer/balboa_worldwide_app/blob/master/doc/protocol.md

# Original README by cribskip

# esp8266_spa
Control for a Balboa spa controller using the esp8266 (tested on BP2100 and BP601 series)

The sketch connects to the tub, gets an ID and spits out the state to the MQTT broker on topics "Spa/#".
You can control the tub using the subscribed topics, f.e. "Spa/light" with message "ON" for maximum compatability with openhab.

Maybe you need to adjust the sketch to your tub configuration (number of pumps, connection of blower, nr of lights...). You may find the DEBUG comments useful for this task.

Bonus: you may add several relays or such like I did ;-)

# Getting started
- Make sure your Spa is using a Balboa controller
- This is known to work on and developed on a BP2100G0 and BP601 series controllers. Yours may be compatible.
- Get the parts
- Flash the esp8266 fom the Arduino IDE or PlatformIO
- Connect everything together
- Hook up on the Spa
- Enjoy and get tubbin' ;-)

# Parts
- Get a esp8266, perferable a Wemos D1 Pro in case you need to attach a seperate antenna
- RS485 bus transceiver, (for example the "ARCELI TTL To RS485 Adapter 485 Serial Port UART Level Converter Module 3.3V 5V")
- A DC-DC converter for powering from the Tub (LM2596 for example)
- breadboard, wire etc...


# Hardware connections
![Example](https://github.com/EmmanuelLM/esp8266_spa/blob/master/esp8266_spa_bb.png)
- Look up finding the right wires on https://github.com/ccutrer/balboa_worldwide_app/wiki#physical-layer
- Connect the DC-DC converter to the supply wires (+ and Ground) from the Tub
- Set the DC-DC converter to output 3.3V. This output voltage (+ Ground) should then connect to the Wemos D1 Mini Pro and the RS485 transceiver
- Connect the RS485 transceiver to the A and B wires
- Connect the esp8266-TX to the RS485 TX
- Connect the esp8266-RX to the RS485 RX

![Example](https://github.com/EmmanuelLM/esp8266_spa/blob/master//PXL_20210101_104120166.jpg)

# Debug
- First, check your voltages - the system is running at 3.3V
- The RX (and to some extent the TX) LEDs of the RS485 transceiver (if using the one above) should light up as data goes through. If that is the case, you know data is being converted from RS485 to TTL
- The Wemos D1 Mini Pro should spit out data to the MQTT broker (MQTT Spy can be useful here to see: 1. that it is connected to the wifi; 2. that it is connected to the broker). If that is the case, you know the device can communicate over MQTT
- Swap A and B - in my personal experience, if A & B are the wrong way round, the hot tub display (if you have one) will display (NO COMM) as the RS485 traffic get garbled by the esp8266_spa

# Appetiser using OpenHab...
![Example](https://github.com/cribskip/esp8266_spa/blob/master/spa_openhab.png)

# HomeAssistant integration
The system uses HomeAssistant autodiscover and should just appear in the MQTT Integration under "Esp Spa"
![Example](https://github.com/EmmanuelLM/esp8266_spa/blob/master//Hassio.png)

# TODO
- Add more documentation
- Add fault reporting
- Add more setting possibilities (filter cycles, preferences maybe)

</details>
