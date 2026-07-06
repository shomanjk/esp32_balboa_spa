# Agent / maintainer context

This file helps AI coding agents and humans work on **`esp32_balboa_spa`** without rediscovering project facts. Update it when architecture, hardware targets, or workflows change.

## Project

- **Purpose:** ESP32 firmware to talk **Balboa** spa controllers: read status (temperature, pumps, etc.) and **send v1 commands** over RS485 via the shared [`spaCommandDispatcher.cpp`](lib/spaMessage/spaCommandDispatcher.cpp) from the **firmware web portal** (`/devices/sci`, interactive **`/status`**, **`/config`**) and **MQTT** (`Spa/<gateway>/cmd/#` → `cmd/result`). See README [MQTT command topics](README.md#mqtt-command-topics-v1).
- **Maintained fork:** Ongoing work lives in this repo; upstream ESP8266-era projects are archival. See [FORK.md](FORK.md) and [README.md](README.md) (“About this fork” + self-contained sections; **collapsed** verbatim snapshot of [NorthernMan54/esp32_balboa_spa](https://github.com/NorthernMan54/esp32_balboa_spa) `ESP32` README at the bottom for lineage).
- **Protocol reference:** [ccutrer/balboa_worldwide_app `doc/protocol.md`](https://github.com/ccutrer/balboa_worldwide_app/blob/master/doc/protocol.md) (linked from README).

## Hardware targets

| Target | PlatformIO env | Notes |
|--------|----------------|--------|
| Generic ESP32 dev board (tub-side RS485) | `ESP32prodOta`, `ESP32ota`, etc. | Default UART pins in `config-example.h`: RX **16**, TX **17**. |
| **M5 Atom Lite + Atomic RS485 Base** | **`M5AtomLite-tub`** | `board = m5stack-atom`. Primary hardware target: stack UART **RX 22** / **TX 19**, **`AUTO_TX true`** — [README](README.md) “M5 Atom Lite + Atomic RS485 Base”, [`src/config-example.h`](src/config-example.h). Optional: [Unit RS485](https://docs.m5stack.com/en/unit/rs485) on Grove **32/26**. Build flag **`M5_ATOM_LED`** enables RGB status / RS485 activity (`src/led_control.*`). |
| **M5 AtomS3 Lite** *(planned)* | *(none yet)* | Bring-up checklist and “do not assume” pins: [GitHub Wiki · Hardware targets](https://github.com/shomanjk/esp32_balboa_spa/wiki/Hardware-targets). **Deferred to ship:** new `pio` env (ESP32-S3 board + USB flags), port or gate **`M5_ATOM_LED`** (likely **M5Unified** vs **`m5stack/M5Atom`**), optional [`spa_module.csv`](spa_module.csv) resize for **8 MB** flash. |
| LilyGo T5 ePaper (remote display) | `ESP32-epd47` | `REMOTE_CLIENT` + `spaEpaper`; separate use case from tub RS485. |

**Tub-side path:** `LOCAL_CLIENT` → RS485 at **115200 8N1** on `Serial2` ([`lib/localRS485Communication/rs485.cpp`](lib/localRS485Communication/rs485.cpp)).

## Configuration (required before a useful build)

- **`src/config.h`** is **gitignored** (see `.gitignore`). Copy from [`src/config-example.h`](src/config-example.h) and set Wi‑Fi, MQTT, **`TX485_Rx` / `TX485_Tx`**, **`AUTO_TX`**. Optional MQTT overrides: **`MQTT_HA_DISCOVERY`**, **`MQTT_DISCOVERY_PREFIX`**, **`MQTT_HA_TEMP_UNIT`** (see [`lib/mqttModule/mqttModule.h`](lib/mqttModule/mqttModule.h) defaults).
- **`VERSION`** string for serial logs: [`src/main.h`](src/main.h) (`#define VERSION`).

## Licensing

- **Firmware** (this repo’s `src/`, `lib/` except vendored carve-outs, build tooling, docs): [PolyForm Noncommercial 1.0.0](LICENSE-firmware). Commercial use requires separate permission.
- **Web UI** (`balboa-spa/` submodule): Apache-2.0 (`balboa-spa/LICENSE`).
- **Overview:** [`LICENSE`](LICENSE) · [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

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
| **`TELNET_LOG`** | Optional. **TelnetStream** on TCP **23** when defined; **`Log`** remains Serial + web ring ([`lib/wifiModule/wifiModule.cpp`](lib/wifiModule/wifiModule.cpp)). Default [`platformio.ini`](platformio.ini) tub/OTA envs **omit** this flag. |
| **`spaEpaper`** | ePaper UI (ESP32-S3 T5 env). |

**Tub-side typical:** `LOCAL_CLIENT` + `LOCAL_CONNECT` + `BRIDGE` (optional **`TELNET_LOG`** for the Telnet listener). **`M5AtomLite-tub`** sets the first three; **`TELNET_LOG`** is off unless you add it to your env.

## Architecture (where to change what)

| Area | Location | Notes |
|------|----------|--------|
| RS485 framing, CTS, CRC, spa **ID** (`0x0A`) | [`lib/localRS485Communication/rs485.cpp`](lib/localRS485Communication/rs485.cpp) | Inbound → `spaReadQueue`; outbound from `spaWriteQueue` on Clear-to-Send. |
| Message parse, status, config requests | [`lib/spaMessage/spaMessage.cpp`](lib/spaMessage/spaMessage.cpp), [`lib/spaMessage/balboa.h`](lib/spaMessage/balboa.h) | `sendMessageToSpa()` enqueues to **`spaWriteQueue`**. |
| MQTT publish | [`lib/spaMessage/spaMqttMessage.cpp`](lib/spaMessage/spaMqttMessage.cpp) | Status/config topics under `Spa/<gateway>/…`. **`faultLog/`** publishes decoded fault code, message, and **`faultLogTime`** (fetched via settings request rotation). **`status/reminderText`** maps status byte 6 via **`reminderTypeMap`** in [`balboa.h`](lib/spaMessage/balboa.h) (e.g. Clean Filter, Check pH, **Change Water** `0x08`); unmapped non-zero codes show **Maintenance reminder**. |
| **Home Assistant MQTT Discovery** | [`lib/mqttModule/haMqttDiscovery.cpp`](lib/mqttModule/haMqttDiscovery.cpp) | After connect: **minimal** discovery (core status only). **Equipment** discovery expands after spa **config** / **information** frames (same rules as web `/status`); retracts optional discovery for slots not in the desired set (clears stale retained broker configs, not only in-RAM this boot). **`availability_topic`** = `Spa/<gateway>/node/state`. Diagnostic **`Gateway WiFi signal`** (`node/rssi`, **`enabled_by_default`: false**). See [HA MQTT Discovery](https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery). |
| MQTT subscribe / commands | [`lib/mqttModule/mqttModule.cpp`](lib/mqttModule/mqttModule.cpp) | Subscribes `Spa/<gateway>/cmd/#` and dispatches commands (`setTemp`, `setTime`, `syncTime`, `mode`, `preset`, `tempUnits`, `filter`, `filter/filter{1,2}/…`, `button/<code>`) through [`spaCommandDispatcher`](lib/spaMessage/spaCommandDispatcher.cpp); publishes command outcome JSON on `Spa/<gateway>/cmd/result`. Filter writes: JSON **`cmd/filter`** (merge from cache) and granular **`HH:MM`** sub-topics. Status telemetry includes **`filter1_running`** / **`filter2_running`**. **`nodeStateReport`** (~90s) publishes gateway diagnostics under **`node/`** (IP, uptime, heap, **`rssi`** when Wi‑Fi connected, etc.). |
| Web / SCI emulation | [`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp) | **`GET /`** serves **Spa Status** (same handler as **`/status`**); **`/state`** is the ESP State page. **`parseBody`**: reads work; **`device_request`** dispatches **`Button`**, **`SetTemp`**, **`SystemTime`**, **`TempUnits`**, **`TimeFormat`**, and **`Filters`** (read via **`Request`/`Filters`**, write via base64 blob → **`spaSetFilterCycles`**) to [`spaCommandDispatcher`](lib/spaMessage/spaCommandDispatcher.cpp) (Balboa **`0x11`** / **`0x20`** / **`0x21`** / **`0x23`** / **`0x27`**) with accepted/rejected XML. Multi-chunk POST body handling is implemented. When no spa data exists yet, `/devices/sci` returns explicit not-ready XML (`ready=false`, `error=no_spa_data_yet`) instead of 404. **`GET /api/status/controls`** feeds live **`/status`** polling (includes **`reminderText`**, **`reminderHint`**). **`/config`** shows decoded fault-log fields when available. JSON: **`GET /api/version`**, **`GET /api/wifi`**, **`GET/POST /api/config/filter`**, **`GET /api/config/export`**, **`POST /api/config/import`**. **Logs:** **`GET /logs`**, **`GET /api/logs?since=&limit=`**, **`WebSocket /api/logs/ws`**, **`GET/POST /api/logs/config`**; capture in [`lib/webLogBuffer/`](lib/webLogBuffer/) (tee on `Serial`; **recursive mutex** serializes tee assembly + ring + Serial so **`Log` from AsyncTCP** (e.g. bridge) does not race **`loop`**). **`/config`**: editable filter schedules + backup/restore toolbar ([`spaConfigExport.cpp`](lib/spaWebServer/spaConfigExport.cpp)). |
| TCP bridge (LAN clients) | [`lib/bridge/bridge.cpp`](lib/bridge/bridge.cpp) | Port **4257**; forwards to RS485 via [`cacheRead.cpp`](lib/spaMessage/cacheRead.cpp) / `sendMessageToSpa`. |
| Remote TCP to spa | [`lib/spaRemoteCommunication/spaCommunication.cpp`](lib/spaRemoteCommunication/spaCommunication.cpp) | Only when **`REMOTE_CLIENT`** is defined. |
| Main loop / init | [`src/main.ino`](src/main.ino) | Conditional compilation per flags above. |

**Queues:** `spaReadQueue` / `spaWriteQueue` — declared in [`lib/spaMessage/spaMessage.cpp`](lib/spaMessage/spaMessage.cpp), exposed via [`src/main.h`](src/main.h).

## Known product gaps (do not assume they work)

1. **LittleFS `balboa-spa` tab** — upstream-style SPA bundle may not expose the same controls as firmware **`/status`**; treat as read-first unless verified for your build.

## Active command-write scope

- **v1 shipped (web + MQTT):** **`0x11`** toggles (including temp range item **80** and heating mode item **81**), **`0x20`** set temperature, **`0x21`** set panel clock time (`setTime`/`syncTime`), **`0x23`** filter cycle schedules (`spaSetFilterCycles` / SCI **`Filters`** / **`/api/config/filter`** / MQTT **`cmd/filter`** + granular sub-topics), and **`0x27`** temp units (`TempUnits`) via shared dispatcher + RS485 **`addCRC`**; **`spaSetTargetTemperature`** enforces protocol setpoint bands; **`spaSetSpaPanelClockTime`** encodes hour/minute per [protocol.md](https://github.com/ccutrer/balboa_worldwide_app/blob/main/doc/protocol.md) Set Time (hour high bit follows current status **24h** flag). **Config backup/restore:** **`GET /api/config/export`**, **`POST /api/config/import`** apply writable sections only; snapshot blocks mismatch without **`force`**. **Filter running MQTT:** **`status/filter1_running`**, **`status/filter2_running`** from live **`filterMode`**.
- **v1 shipped (web + config export/import only):** panel clock format **`TimeFormat`** (`12`/`24`, Balboa **`0x21`** piggyback via **`spaSetSpaPanelClockFormat`**) — SCI **`TimeFormat`**, **`/status`** controls, export/import **`clockFormat`**. No MQTT **`cmd/timeFormat`** topic yet.
- **v1 deferred:** MQTT panel clock format command; broader settings writes beyond the families above (e.g. raw **`0x2E`** configuration, **`0x26`** preferences beyond temp scale).
- **Protocol authority for wire behavior:** [ccutrer/balboa_worldwide_app `doc/protocol.md`](https://github.com/ccutrer/balboa_worldwide_app/blob/main/doc/protocol.md); validate frame bytes and payload semantics there before enabling each new command family.

## Product roadmap

Planned and deferred **product** direction lives in the README so it stays visible to contributors and users: [Roadmap](README.md#roadmap). **AGENTS.md** stays focused on how the code is structured, how to build it, and agent-oriented gaps (command scope, known bugs).

## Testing

- **Bench:** [`emulator/spaEmulator.js`](emulator/spaEmulator.js) + [`emulator/README.md`](emulator/README.md) for crude serial injection.
- **Logs:** `LOG_LEVEL_VERBOSE` in `platformio.ini` `[com]` `build_flags`.
- **Post-panic / bridge field breadcrumbs:** **`DIAG_FAULT_CAPTURE`** (compile-time; **on** for [`env:M5AtomLite-tub`](platformio.ini) / **`M5AtomLite-tub-ota`**, **off** elsewhere): RTC slow-memory ring of structured fault entries (survives panic reboot), appended on panic/WDT boots and selected bridge/queue failures; **`esp_system_abort()`** text via **`--wrap=esp_system_abort`** (next boot **`faultLog`** entry); **`GET /api/version`** exposes **`faultLog`** as JSON objects (`uptimeMs`, `msg`, optional `wallUnix`/`wallTime`, plus **`deviceUptimeMs`**) and **`lastBridgeIngress`**. Implementation: [`lib/faultCapture/`](lib/faultCapture/).
- **Command-write debug ledger:** keep attempted wire-level fixes and outcomes in [`docs/command-write-debug-log.md`](docs/command-write-debug-log.md) to avoid duplicate troubleshooting passes.
- **Bridge artifact tracking:** keep hand-authored bridge inputs in git (`*.matrix.json`, `*.example.json`, curated fixtures/playbooks). Generated run outputs (`*-live-run.json`, `*-dry-run.json`, `*-diff-*.json`, `bridge-raw-last-run.json`) are treated as disposable diagnostics and should stay ignored unless intentionally curated for a specific doc/debug checkpoint. Same for live gateway captures: **`docs/diag-*.json`** and **`docs/telnet-*.txt`** (gitignored; example shape in [`docs/diag-light1-next-cts-live-run.example.json`](docs/diag-light1-next-cts-live-run.example.json)).

## Flashing (PlatformIO CLI)

1. **`PATH`:** If `pio` is not found, use `~/.platformio/penv/bin/pio` or add `export PATH="$HOME/.platformio/penv/bin:$PATH"` to `~/.zshrc`.
2. **Local config:** Copy [`src/config-example.h`](src/config-example.h) → `src/config.h` (Wi‑Fi/MQTT/RS485 pins) and [`platformio_local.ini.example`](platformio_local.ini.example) → `platformio_local.ini` (USB/OTA ports; gitignored).
3. **Build:** `pio run -e M5AtomLite-tub` (first build may fetch toolchains and run the LittleFS `balboa-spa` pre-build — requires Node/npm if the web bundle is built).
4. **USB upload:** Connect the Atom; then `pio run -e M5AtomLite-tub -t upload` (ports from `platformio_local.ini` or `pio run ... -t upload --upload-port /dev/cu.…` if needed).
5. **Filesystem (web UI):** `pio run -e M5AtomLite-tub -t uploadfs` after firmware, if you use the bundled web assets.
6. **Serial monitor:** `pio device monitor -e M5AtomLite-tub -b 115200`.

## Releases & versioning

- **Changelog:** [`CHANGELOG.md`](CHANGELOG.md); **`[Unreleased]`** for pending work.
- **Firmware version:** [`src/main.h`](src/main.h) `VERSION` should align with tagged releases when cutting a release.
- **Process:** [FORK.md](FORK.md) (push, optional PRs on fork, tags, GitHub Releases).

## Conventions for agents

- **Git:** Do **not** run **`git commit`**, **`git push`**, or push-implying **`gh`** steps unless the user **explicitly** asks in the same request. Summarize edits and let the maintainer commit/push.
- Prefer **small, focused changes**; match existing style and naming.
- **Never commit `.claude/`** (local Claude / agent worktrees) or **`.cursor/`** (local Cursor rules); both are **gitignored**. Accidental gitlinks under `.claude/` broke `git submodule update` ([issue #6](https://github.com/shomanjk/esp32_balboa_spa/issues/6)).
- **`config.h`** secrets: never commit; use **`config-example.h`** for templates only.
- When changing SPA behavior, update both places intentionally:
  - SPA source in submodule: `balboa-spa/src/...` (commit/push in SPA fork)
  - Firmware/API compatibility in parent repo: `lib/spaWebServer/...`, docs/changelog as needed
- After user-facing behavior or release steps change, update **CHANGELOG** `[Unreleased]` and this file if needed.
