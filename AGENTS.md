# Agent / maintainer context

This file helps AI coding agents and humans work on **`esp32_balboa_spa`** without rediscovering project facts. Update it when architecture, hardware targets, or workflows change.

## Project

- **Purpose:** ESP32 firmware to talk **Balboa** spa controllers: read status (temperature, pumps, etc.) and (when implemented) send commands over the Balboa serial protocol.
- **Maintained fork:** Ongoing work lives in this repo; upstream ESP8266-era projects are archival. See [FORK.md](FORK.md) and [README.md](README.md) (“About this fork” + self-contained sections; **collapsed** verbatim snapshot of [NorthernMan54/esp32_balboa_spa](https://github.com/NorthernMan54/esp32_balboa_spa) `ESP32` README at the bottom for lineage).
- **Protocol reference:** [ccutrer/balboa_worldwide_app `doc/protocol.md`](https://github.com/ccutrer/balboa_worldwide_app/blob/master/doc/protocol.md) (linked from README).

## Hardware targets

| Target | PlatformIO env | Notes |
|--------|----------------|--------|
| Generic ESP32 dev board (tub-side RS485) | `ESP32prodOta`, `ESP32ota`, etc. | Default UART pins in `config-example.h`: RX **16**, TX **17**. |
| **M5 Atom Lite + Atomic RS485 Base** | **`M5AtomLite-tub`** | `board = m5stack-atom`. Primary hardware target: stack UART **RX 22** / **TX 19**, **`AUTO_TX true`** — [README](README.md) “M5 Atom Lite + Atomic RS485 Base”, [`src/config-example.h`](src/config-example.h). Optional: [Unit RS485](https://docs.m5stack.com/en/unit/rs485) on Grove **32/26**. Build flag **`M5_ATOM_LED`** enables RGB status / RS485 activity (`src/led_control.*`). |
| LilyGo T5 ePaper (remote display) | `ESP32-epd47` | `REMOTE_CLIENT` + `spaEpaper`; separate use case from tub RS485. |

**Tub-side path:** `LOCAL_CLIENT` → RS485 at **115200 8N1** on `Serial2` ([`lib/localRS485Communication/rs485.cpp`](lib/localRS485Communication/rs485.cpp)).

## Configuration (required before a useful build)

- **`src/config.h`** is **gitignored** (see `.gitignore`). Copy from [`src/config-example.h`](src/config-example.h) and set Wi‑Fi, MQTT, **`TX485_Rx` / `TX485_Tx`**, **`AUTO_TX`**. Optional MQTT overrides: **`MQTT_HA_DISCOVERY`**, **`MQTT_DISCOVERY_PREFIX`**, **`MQTT_HA_TEMP_UNIT`** (see [`lib/mqttModule/mqttModule.h`](lib/mqttModule/mqttModule.h) defaults).
- **`VERSION`** string for serial logs: [`src/main.h`](src/main.h) (`#define VERSION`).

## Build system

- **PlatformIO** ([`platformio.ini`](platformio.ini)). Web UI assets: `data_dir = balboa-spa/dist`, LittleFS, partition **`spa_module.csv`** (root).
- **Default env:** `default_envs = ESP32ota` (often **not** tub-side; check flags in each `[env:…]`).
- **Web SPA source:** `balboa-spa` is a tracked **git submodule** pinned in this repo, with fork origin `https://github.com/shomanjk/balboa-spa.git` and upstream `https://github.com/jozefnad/balboa-spa.git`.
- **First clone/setup:** run `git submodule update --init --recursive` before `uploadfs` builds.

### Compile-time flags (see README “Compiler Definitions”)

| Flag | Role |
|------|------|
| **`LOCAL_CLIENT`** | RS485 to spa controller (tub-side). |
| **`LOCAL_CONNECT`** | UDP discovery (port 30303) for Balboa-style discovery. |
| **`BRIDGE`** | TCP server on **4257**; pairs with Homebridge [homebridge-plugin-bwaspa](https://github.com/vincedarley/homebridge-plugin-bwaspa). |
| **`REMOTE_CLIENT`** | TCP **client** to port 4257 (remote/kitchen device), not tub RS485. |
| **`TELNET_LOG`** | Telnet logging. |
| **`spaEpaper`** | ePaper UI (ESP32-S3 T5 env). |

**Tub-side typical:** `LOCAL_CLIENT` + `LOCAL_CONNECT` + `BRIDGE` (+ optional `TELNET_LOG`). **`M5AtomLite-tub`** already sets these.

## Architecture (where to change what)

| Area | Location | Notes |
|------|----------|--------|
| RS485 framing, CTS, CRC, spa **ID** (`0x0A`) | [`lib/localRS485Communication/rs485.cpp`](lib/localRS485Communication/rs485.cpp) | Inbound → `spaReadQueue`; outbound from `spaWriteQueue` on Clear-to-Send. |
| Message parse, status, config requests | [`lib/spaMessage/spaMessage.cpp`](lib/spaMessage/spaMessage.cpp), [`lib/spaMessage/balboa.h`](lib/spaMessage/balboa.h) | `sendMessageToSpa()` enqueues to **`spaWriteQueue`**. |
| MQTT publish | [`lib/spaMessage/spaMqttMessage.cpp`](lib/spaMessage/spaMqttMessage.cpp) | Status/config topics under `Spa/<gateway>/…`. |
| **Home Assistant MQTT Discovery** | [`lib/mqttModule/haMqttDiscovery.cpp`](lib/mqttModule/haMqttDiscovery.cpp) | After connect: **minimal** discovery (core status only). **Equipment** discovery expands after spa **config** / **information** frames (same rules as web `/status`); retracts optional discovery for slots not in the desired set (clears stale retained broker configs, not only in-RAM this boot). **`availability_topic`** = `Spa/<gateway>/node/state`. See [HA MQTT Discovery](https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery). |
| MQTT subscribe / commands | [`lib/mqttModule/mqttModule.cpp`](lib/mqttModule/mqttModule.cpp) | **Known gap:** callback currently **echoes** payloads; commands not implemented. |
| Web / SCI emulation | [`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp) | **`parseBody`**: reads work; **`device_request` / buttons** not fully wired. Multi-chunk POST body handling is implemented. When no spa data exists yet, `/devices/sci` returns explicit not-ready XML (`ready=false`, `error=no_spa_data_yet`) instead of 404. JSON: **`GET /api/version`**, **`GET /api/wifi`** (Wi‑Fi status/RSSI for `/state` live section + chart). **Logs:** **`GET /logs`** (portal page), **`GET /api/logs?since=&limit=`**, **`WebSocket /api/logs/ws`**, **`GET/POST /api/logs/config`** (runtime log level); capture in [`lib/webLogBuffer/`](lib/webLogBuffer/) (tee on `Serial`). |
| TCP bridge (LAN clients) | [`lib/bridge/bridge.cpp`](lib/bridge/bridge.cpp) | Port **4257**; forwards to RS485 via [`cacheRead.cpp`](lib/spaMessage/cacheRead.cpp) / `sendMessageToSpa`. |
| Remote TCP to spa | [`lib/spaRemoteCommunication/spaCommunication.cpp`](lib/spaRemoteCommunication/spaCommunication.cpp) | Only when **`REMOTE_CLIENT`** is defined. |
| Main loop / init | [`src/main.ino`](src/main.ino) | Conditional compilation per flags above. |

**Queues:** `spaReadQueue` / `spaWriteQueue` — declared in [`lib/spaMessage/spaMessage.cpp`](lib/spaMessage/spaMessage.cpp), exposed via [`src/main.h`](src/main.h).

## Known product gaps (do not assume they work)

1. **MQTT commands** — subscribe exists; handler does not dispatch commands to `sendMessageToSpa`.
2. **Web UI buttons** — README states not wired; `parseBody` logs `device_request` but does not send toggles.
3. **Command framing** — implement Balboa **0x11** (toggle) / **0x20** (set temp) etc. with correct CRC; reuse patterns from [`rs485.cpp`](lib/localRS485Communication/rs485.cpp) `addCRC` / [`balboa.h`](lib/spaMessage/balboa.h) prebuilt frames.

## Active command-write scope

- **v1 in scope:** web + MQTT command path for Balboa **`0x11`** (toggle/button) and **`0x20`** (set temperature) only.
- **Deferred after v1:** `SystemTime`, `TimeFormat`, `TempUnits`, and broader settings writes.
- **Protocol authority for wire behavior:** [ccutrer/balboa_worldwide_app `doc/protocol.md`](https://github.com/ccutrer/balboa_worldwide_app/blob/main/doc/protocol.md); validate frame bytes and payload semantics there before enabling each new command family.

## Roadmap (deferred)

- **ePaper temperature UOM** — When building with **`spaEpaper`**, align [`lib/spaEpaper/spaEpaper.cpp`](lib/spaEpaper/spaEpaper.cpp) labels and chart titles with **`spaStatusData.tempScale`** (same °F/°C and decimal rules as the web `/status` page).

## Testing

- **Bench:** [`emulator/spaEmulator.js`](emulator/spaEmulator.js) + [`emulator/README.md`](emulator/README.md) for crude serial injection.
- **Logs:** `LOG_LEVEL_VERBOSE` in `platformio.ini` `[com]` `build_flags`.
- **Command-write debug ledger:** keep attempted wire-level fixes and outcomes in [`docs/command-write-debug-log.md`](docs/command-write-debug-log.md) to avoid duplicate troubleshooting passes.
- **Bridge artifact tracking:** keep hand-authored bridge inputs in git (`*.matrix.json`, `*.example.json`, curated fixtures/playbooks). Generated run outputs (`*-live-run.json`, `*-dry-run.json`, `*-diff-*.json`, `bridge-raw-last-run.json`) are treated as disposable diagnostics and should stay ignored unless intentionally curated for a specific doc/debug checkpoint.

## Flashing (PlatformIO CLI)

1. **`PATH`:** If `pio` is not found, use `~/.platformio/penv/bin/pio` or add `export PATH="$HOME/.platformio/penv/bin:$PATH"` to `~/.zshrc`.
2. **`src/config.h`:** Copy from [`src/config-example.h`](src/config-example.h); set Wi‑Fi/MQTT and RS485 pins for your hardware (see [README](README.md)).
3. **Build:** `pio run -e M5AtomLite-tub` (first build may fetch toolchains and run the LittleFS `balboa-spa` pre-build — requires Node/npm if the web bundle is built).
4. **USB upload:** Connect the Atom; then `pio run -e M5AtomLite-tub -t upload` (set `upload_port` in [`platformio.ini`](platformio.ini) or use `pio run ... -t upload --upload-port /dev/cu.…` if needed).
5. **Filesystem (web UI):** `pio run -e M5AtomLite-tub -t uploadfs` after firmware, if you use the bundled web assets.
6. **Serial monitor:** `pio device monitor -e M5AtomLite-tub -b 115200`.

## Releases & versioning

- **Changelog:** [`CHANGELOG.md`](CHANGELOG.md); **`[Unreleased]`** for pending work.
- **Firmware version:** [`src/main.h`](src/main.h) `VERSION` should align with tagged releases when cutting a release.
- **Process:** [FORK.md](FORK.md) (push, optional PRs on fork, tags, GitHub Releases).

## Conventions for agents

- Prefer **small, focused changes**; match existing style and naming.
- **`config.h`** secrets: never commit; use **`config-example.h`** for templates only.
- When changing SPA behavior, update both places intentionally:
  - SPA source in submodule: `balboa-spa/src/...` (commit/push in SPA fork)
  - Firmware/API compatibility in parent repo: `lib/spaWebServer/...`, docs/changelog as needed
- After user-facing behavior or release steps change, update **CHANGELOG** `[Unreleased]` and this file if needed.
