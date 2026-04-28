# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
where version numbers are used.

## [Unreleased]

## [1.4.2] - 2026-04-27

### Changed

- **Web portal ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)):** **`/config`** and **`/state`** show **local date/time** for spa **`lastUpdate`** / **`lastRequest`** and ESP wall-clock fields, with optional **Raw epoch (Unix s)** details (same pattern as **`/status`**). **Filter Configuration** adds short scheduling help, **`lastUpdate (filter settings)`**, and **`Filter 2 enabled`** when filter data has been received.
- **`src/main.h`:** Firmware **`VERSION`** **1.4.2**.
- **`lib/Analytics/Analytics.h`:** **`ANALYTICS_VERSION`** **1.4.2**.

## [1.4.1] - 2026-04-27

### Changed

- **README:** Restructured for a self-contained top (lineage, features, web/MQTT/HA, build, M5, OTA, compiler flags, credits) and a **collapsed verbatim snapshot** of [NorthernMan54/esp32_balboa_spa](https://github.com/NorthernMan54/esp32_balboa_spa) branch **`ESP32`** README at the bottom; [FORK.md](FORK.md) lineage updated to name NorthernMan54.
- **Home Assistant MQTT discovery:** On connect, publishes a **minimal** retained set (temperatures, heating state, spa modes, locks, clock, temp scale). **Pumps, loads, mister, and diagnostic model/software** discovery is published only after **`spaConfigurationData`** / **`spaInformationData`** are populated (same install rules as the web `/status` page); slots that drop out of the desired set are **retracted** (empty retained config) for the current power session. Triggered from configuration and information parses in [`lib/spaMessage/spaMessage.cpp`](lib/spaMessage/spaMessage.cpp); logic in [`lib/mqttModule/haMqttDiscovery.cpp`](lib/mqttModule/haMqttDiscovery.cpp).
- **`src/main.h`:** Firmware version **`VERSION`** set to **1.4.1**.
- **`lib/Analytics/Analytics.h`:** **`ANALYTICS_VERSION`** aligned with **`VERSION`** (**1.4.1**).

## [1.4.0] - 2026-04-27

### Added

- **Home Assistant MQTT Discovery:** On MQTT connect, the gateway publishes retained discovery configs under `homeassistant/…/config` for spa status (temperatures, pumps, modes, etc.), binary sensors for loads using `On`/`Off` payloads, and diagnostic controller model/software ID topics. Availability ties to `Spa/<gateway>/node/state`. Optional `config.h` overrides: `MQTT_HA_DISCOVERY`, `MQTT_DISCOVERY_PREFIX`, `MQTT_HA_TEMP_UNIT`. Implementation: [`lib/mqttModule/haMqttDiscovery.cpp`](lib/mqttModule/haMqttDiscovery.cpp); PubSubClient buffer increased to **4096** in [`lib/mqttModule/mqttModule.cpp`](lib/mqttModule/mqttModule.cpp).
- **`src/main.h`:** Firmware version **`VERSION`** set to **1.4.0**.
- **`lib/Analytics/Analytics.h`:** **`ANALYTICS_VERSION`** aligned with **`VERSION`** (**1.4.0**).

## [1.3.0] - 2026-04-27

### Changed

- **Web portal `/status`:** **lastUpdate** shows local date/time when NTP time is valid, **Time not synced** when epoch is 0, with optional **Raw epoch** details; **Heating state** uses text labels from **`heatingStateMap`**; **magicNumber** has a short caption (firmware RAM marker `0x12345678`, not spa model).
- **`src/main.h`:** Firmware version **`VERSION`** set to **1.3.0**.
- **`lib/Analytics/Analytics.h`:** **`ANALYTICS_VERSION`** aligned with **`VERSION`** (**1.3.0**).

## [1.2.0] - 2026-04-27

### Changed

- **Web portal `/status`:** Equipment tiles use **`spaConfigurationData`** (when received) to de-emphasize items marked **not installed** in the spa configuration (pumps, lights, circ, blower, mister).
- **`src/main.h`:** Firmware version **`VERSION`** set to **1.2.0**.
- **`lib/Analytics/Analytics.h`:** **`ANALYTICS_VERSION`** aligned with **`VERSION`** (**1.2.0**).

## [1.1.0] - 2026-04-27

### Changed

- **Web portal `/status`:** Sectioned responsive layout; temperatures use spa **F vs C** (`tempScale`) for suffix and decimals (°F whole degrees, °C one decimal); human-readable **Temp scale**; **Histories** with canvas charts (temperature; heater min/day; filter hr/day) and collapsible raw lists.
- **`platformio.ini` `[env:ESP32ota]`:** Re-enabled `LOCAL_CONNECT`, `LOCAL_CLIENT`, and `BRIDGE` so the default OTA env matches tub-side builds (fixes compile after role flags were commented out in error).
- **balboa-spa (login):** Balboa BWA login screen shows a short note that on the local gateway, username and password are not verified and any values may be entered to continue.
- **`src/main.h`:** Firmware version **`VERSION`** set to **1.1.0**.
- **`lib/Analytics/Analytics.h`:** **`ANALYTICS_VERSION`** aligned with **`VERSION`** (**1.1.0**).

## [1.0.0] - 2026-04-27

First **1.x** release: tub-side RS485 path validated on hardware; correctness fixes to frame parsing and safer UART draining so the spa link stays reliable under load.

### Fixed

- **RS485 frame validation (critical):** The 4-byte prelude check used **bitwise OR** (`|`) instead of **logical OR** (`||`) when combining the length test with the broadcast-flag test (`0xBF` / `0xAF`), which could corrupt accept/reject decisions for Balboa frames. This is corrected to logical OR.
- **RS485 bounds safety:** Double-`0x7E` handling and length overrun checks now verify `spaMessage.size()` before indexing `spaMessage[1]` or comparing `size() - 2` to the length byte, avoiding invalid reads on short buffers.
- **RS485 end-of-frame with batched reads:** After refactoring UART handling to drain multiple bytes per loop, complete-frame detection now runs for **each** processed `0x7E` so frames still complete correctly when several bytes arrive in one iteration.

### Added

- **RS485 raw UART capture (`GET /api/rs485/raw`):** Bounded raw-byte ring buffer for remote diagnostics: hex stream, per-byte gap timing, active polarity mode, and UART backlog at capture time.
- **RS485 marker/backlog diagnostics (`/state` + `GET /api/rs485`):** `0x7E` frame-marker counters, max UART backlog, raw-capture overflow counters, and effective UART pin/baud/AUTO_TX reporting.

### Changed

- **RS485 UART drain:** `rs485Loop()` drains a bounded batch of waiting UART bytes per pass (instead of one), reducing backlog when the main loop is briefly blocked.
- **RS485 persisted stats:** `RS_485_MAGIC_NUMBER` incremented so upgraded firmware does not misinterpret older in-flash layout after struct growth.
- **MQTT reconnect behavior:** Reconnect attempts throttled (30s), shorter socket timeout, `setBufferSize(512)` applied at setup; avoids an unreachable broker dominating loop time and starving RS485.
- **`src/main.h`:** Firmware version **`VERSION`** set to **1.0.0**.
- **`lib/Analytics/Analytics.h`:** **`ANALYTICS_VERSION`** aligned with **`VERSION`** (**1.0.0**).

## [0.7.2] - 2026-04-27

### Added

- **RS485 per-mode diagnostics (`/state` + `GET /api/rs485`):** Added `rawBytesNormal*` and `rawBytesInverted*` counters so troubleshooting can compare traffic quality while auto-detect is in `normal` vs `inverted_rx_tx` mode.

### Changed

- **`src/main.h`:** Firmware version **`VERSION`** set to **0.7.2**.
- **`lib/Analytics/Analytics.h`:** **`ANALYTICS_VERSION`** aligned with **`VERSION`** (**0.7.2**).

## [0.7.1] - 2026-04-27

### Added

- **RS485 mode visibility (`/state` + `GET /api/rs485`):** Exposed active RS485 mode (`normal` or `inverted_rx_tx`) and detect phase to support remote troubleshooting over Wi-Fi.

### Changed

- **`lib/localRS485Communication/rs485.cpp`:** Updated auto-detect to test both `normal` and `inverted_rx_tx` (virtual A/B swap via RX/TX inversion) and only lock after a first valid frame.
- **`src/main.h`:** Firmware version **`VERSION`** set to **0.7.1**.
- **`lib/Analytics/Analytics.h`:** **`ANALYTICS_VERSION`** aligned with **`VERSION`** (**0.7.1**).

## [0.7.0] - 2026-04-27

### Added

- **RS485 diagnostics (`/state` + `GET /api/rs485`):** Added raw UART byte counters, frame counters, last byte timestamp, and last valid frame timestamp for remote troubleshooting. Added a simple RS485 health classification (`NO_UART_BYTES`, `UART_BYTES_NO_VALID_FRAMES`, `VALID_FRAMES_OK`) exposed via JSON.

### Changed

- **`lib/localRS485Communication/rs485.cpp`:** Hardened parser flow to only process frame-end checks when a new UART byte is read, reducing stale-byte edge cases during low/no traffic.
- **`src/main.h`:** Firmware version **`VERSION`** set to **0.7.0**.
- **`lib/Analytics/Analytics.h`:** **`ANALYTICS_VERSION`** aligned with **`VERSION`** (**0.7.0**).

## [0.6.0] - 2026-04-27

### Added

- **`lib/localRS485Communication/rs485.cpp`:** Added RS485 polarity auto-detect fallback for tub-side UART bring-up. Firmware now starts with normal UART polarity, retries with inverted RX polarity if no valid Balboa frames are seen during the detect window, and logs/records lock state and polarity switch stats.

### Changed

- **`src/main.h`:** Firmware version **`VERSION`** set to **0.6.0**.
- **`lib/Analytics/Analytics.h`:** **`ANALYTICS_VERSION`** aligned with **`VERSION`** (**0.6.0**).

## [0.5.0] - 2026-03-27

### Changed

- **Legacy web portal (`/status`, `/config`, `/state`):** mobile-first responsive refresh for firmware-served pages in `lib/spaWebServer/spaWebServer.cpp` with viewport meta tag, wrapping/stacking nav buttons, card-like section layout, improved text wrapping for long diagnostics, and fluid media sizing for the panel image and Wi-Fi RSSI chart (including chart resize handling on viewport/orientation changes).
- **SPA dependency management:** `balboa-spa` is now tracked as a git submodule pinned in this repository (`.gitmodules`) instead of being silently cloned at build time.
- **Build pre-action (`scripts/extra_script.py`):** now requires an initialized submodule and fails fast with guidance (`git submodule update --init --recursive`) if missing; removed implicit clone behavior.
- **Documentation:** `README.md` and `AGENTS.md` now document the submodule workflow (`origin` fork + `upstream` remote) and explicitly call out that SPA behavior changes should be coordinated between submodule source and firmware API compatibility.
- **`src/main.h`:** Firmware version **`VERSION`** set to **0.5.0**.
- **`lib/Analytics/Analytics.h`:** **`ANALYTICS_VERSION`** aligned with **`VERSION`** (**0.5.0**).

## [0.4.0] - 2026-03-27

### Added

- **Web ESP State / Wi‑Fi:** `/state` includes a **WiFi** section (status, SSID, hostname, IP, gateway, subnet, DNS, channel, MAC, RSSI), **RSSI quality label/color**, **5‑minute average RSSI**, and a **rolling RSSI chart** (browser polls `GET /api/wifi` every 5s, ~5 minute window). The chart is right-aligned (newest sample at right), with amber/red threshold guides at -75/-80 dBm. New JSON endpoint **`GET /api/wifi`** returns connection fields for the live chart and updates.

### Changed

- **`src/main.h`:** Firmware version **`VERSION`** set to **0.4.0**.
- **`lib/Analytics/Analytics.h`:** **`ANALYTICS_VERSION`** aligned with **`VERSION`** (**0.4.0**).

## [0.3.1] - 2026-03-26

### Added

- **Web firmware metadata endpoint:** Added `GET /api/version` in `lib/spaWebServer/spaWebServer.cpp` returning firmware `version`, `build`, `hostname`, `ip`, and `restartReason`.

### Changed

- **Web state page:** `/state` now shows `Firmware Version` and `Firmware Build` to simplify OTA verification after updates.
- **`src/main.h`:** Firmware version **`VERSION`** set to **0.3.1**.
- **`lib/Analytics/Analytics.h`:** **`ANALYTICS_VERSION`** aligned with **`VERSION`** (**0.3.1**).

## [0.3.0] - 2026-03-26

### Added

- **M5 Atom Lite (`M5AtomLite-tub`):** Optional RGB LED feedback (`led_control` / **`M5_ATOM_LED`**) — Wi‑Fi connected (green) / disconnected (red), brief blue/yellow flashes around RS485 loop activity (from archived **`m5stack-8`** work). Git tag **`archive/m5stack-8`** points at the pre-merge snapshot for reference.
- **OTA env for M5 tub-side:** Added **`M5AtomLite-tub-ota`** PlatformIO environment using `espota` upload protocol for wireless firmware updates.

### Changed

- **`lib/localRS485Communication/rs485.cpp`:** UART for RS485 is **`RS485_SERIAL_PORT`** (default **`Serial2`** via `config.h` / `config-example.h`).
- **`lib/wifiModule/wifiModule.cpp` / `lib/wifiModule/wifiModule.h`:** OTA hardening updates:
  - optional OTA auth controls (`ENABLE_OTA_AUTH`, `OTA_PASSWORD`) with trusted-LAN default,
  - configurable OTA timeout (`OTA_TIMEOUT_MS`),
  - clearer progress/error logs and restart-reason breadcrumbs for OTA lifecycle.
- **`src/config-example.h`:** Added OTA config templates (`ENABLE_OTA_AUTH`, `OTA_PASSWORD`, `OTA_TIMEOUT_MS`).
- **`src/main.h`:** Firmware version **`VERSION`** set to **0.3.0**.
- **`lib/Analytics/Analytics.h`:** **`ANALYTICS_VERSION`** aligned with **`VERSION`** (**0.3.0**).

### Documentation

- **[README.md](README.md):** M5 tub-side section describes RGB LED meaning (**`M5_ATOM_LED`**) with a color table and links to **`led_control`**; compiler definitions list includes **`M5_ATOM_LED`**.
- **[README.md](README.md):** Added OTA update section for `M5AtomLite-tub-ota`, including trusted-LAN default and recovery guidance.

## [0.2.0] - 2026-03-26

Tub-side docs now target the **[M5 Atomic RS485 Base](https://docs.m5stack.com/en/atom/Atomic%20RS485%20Base)** stack; firmware **`VERSION`** bumped for this release.

### Added

- **[AGENTS.md](AGENTS.md):** Maintainer and AI-agent context (hardware targets, PlatformIO envs, compile flags, key source files, known gaps, release/version notes).

### Changed

- **`src/main.h`:** Firmware version **`VERSION`** set to **0.2.0** (serial logs and build identity).
- **`lib/Analytics/Analytics.h`:** **`ANALYTICS_VERSION`** aligned with **`VERSION`** (0.2.0).
- **[README.md](README.md):** Tub-side section retitled and rewritten for **Atom Lite + Atomic RS485 Base** (stacked UART **GPIO 22/19** per M5’s [Arduino example](https://github.com/m5stack/M5-ProductExampleCodes/blob/master/AtomBase/AtomicRS485/Arduino/AtomicRS485/AtomicRS485.ino), **12 V→5 V** / **VH‑3.96** / optional **120 Ω** termination, Balboa physical-layer link). **[M5 Unit RS485](https://docs.m5stack.com/en/unit/rs485)** (Grove **32/26**) documented as an alternate.
- **`src/config-example.h`:** **Recommended** M5 block is **Atomic RS485 Base** (**`TX485_Rx` 22**, **`TX485_Tx` 19**, **`AUTO_TX` true**); **Unit RS485** Grove wiring moved to a separate “alternate” comment block.
- **[AGENTS.md](AGENTS.md):** Hardware table lists **Atomic RS485 Base** as the primary **`M5AtomLite-tub`** target (22/19).
- **[platformio.ini](platformio.ini):** Comment for **`M5AtomLite-tub`** references the Atomic RS485 Base and README.

## [0.1.0] - 2026-03-26

First **tagged release of this maintained fork** (lineage and workflow: [FORK.md](FORK.md)). Not a claim of greenfield authorship; version tracks releases from this repository only.

### Added

- **Fork documentation:** [README.md](README.md) **About this fork** section; [FORK.md](FORK.md) describing lineage, that upstream archival repos are not the PR target, and recommended **git push / optional PRs / tags** workflow for this fork.
- **M5 Atom Lite (tub-side):** PlatformIO environment `M5AtomLite-tub` using `board = m5stack-atom`, tub-side build flags (`LOCAL_CLIENT`, `LOCAL_CONNECT`, `BRIDGE`, `TELNET_LOG`), and `upload_speed = 1500000` ([`platformio.ini`](platformio.ini)).
- **`src/config-example.h`:** Documented optional UART/GPIO settings for **M5 Atom Lite + M5 Unit RS485** (Grove wiring, `TX485_Rx` / `TX485_Tx`, `AUTO_TX` guidance) with links to M5 documentation.
- **`README.md`:** Section **“M5 Atom Lite + M5 RS485 (tub-side)”** describing the new env, wiring to [Unit RS485](https://docs.m5stack.com/en/unit/rs485), `config.h` setup, and power notes.

### Changed

- **`src/config-example.h`:** Clarified RS485 pin comments for generic ESP32 vs M5; default GPIO16/17 retained for existing setups.

[Unreleased]: https://github.com/shomanjk/esp32_balboa_spa/compare/v1.4.2...HEAD
[1.4.2]: https://github.com/shomanjk/esp32_balboa_spa/compare/v1.4.1...v1.4.2
[1.4.1]: https://github.com/shomanjk/esp32_balboa_spa/compare/v1.4.0...v1.4.1
[1.4.0]: https://github.com/shomanjk/esp32_balboa_spa/compare/v1.3.0...v1.4.0
[1.3.0]: https://github.com/shomanjk/esp32_balboa_spa/releases/tag/v1.3.0
[1.2.0]: https://github.com/shomanjk/esp32_balboa_spa/releases/tag/v1.2.0
[1.1.0]: https://github.com/shomanjk/esp32_balboa_spa/releases/tag/v1.1.0
[1.0.0]: https://github.com/shomanjk/esp32_balboa_spa/releases/tag/v1.0.0
[0.7.2]: https://github.com/shomanjk/esp32_balboa_spa/releases/tag/v0.7.2
[0.7.1]: https://github.com/shomanjk/esp32_balboa_spa/releases/tag/v0.7.1
[0.7.0]: https://github.com/shomanjk/esp32_balboa_spa/releases/tag/v0.7.0
[0.6.0]: https://github.com/shomanjk/esp32_balboa_spa/releases/tag/v0.6.0
[0.5.0]: https://github.com/shomanjk/esp32_balboa_spa/releases/tag/v0.5.0
[0.4.0]: https://github.com/shomanjk/esp32_balboa_spa/releases/tag/v0.4.0
[0.3.1]: https://github.com/shomanjk/esp32_balboa_spa/releases/tag/v0.3.1
[0.3.0]: https://github.com/shomanjk/esp32_balboa_spa/releases/tag/v0.3.0
[0.2.0]: https://github.com/shomanjk/esp32_balboa_spa/releases/tag/v0.2.0
[0.1.0]: https://github.com/shomanjk/esp32_balboa_spa/releases/tag/v0.1.0
