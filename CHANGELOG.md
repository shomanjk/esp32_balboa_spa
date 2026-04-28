# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
where version numbers are used.

## [Unreleased]

### Added

- **Diagnostic toggle probe endpoint** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp), [`lib/spaMessage/spaCommandDispatcher.cpp`](lib/spaMessage/spaCommandDispatcher.cpp)): Added `GET /api/diag/toggle` for low-risk A/B command-frame testing (`item`, optional `dest=wifi|id`, optional `pad=00|none`), returning the exact queued frame bytes and acceptance result to isolate controller-specific toggle semantics.
- **Timed toggle sequence diagnostics** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Added `GET /api/diag/toggle_sequence` to execute controlled retry/timing tests (`repeats`, `gap_ms`, `observe_ms`) with per-attempt frame metadata and before/after control snapshots.
- **Command-write debug ledger** ([`docs/command-write-debug-log.md`](docs/command-write-debug-log.md), [`README.md`](README.md), [`AGENTS.md`](AGENTS.md)): Added a persistent troubleshooting log of attempted fixes, observed outcomes, and next diagnostic decision points to avoid duplicate effort.
- **Bridge-first raw harness** ([`scripts/bridge_raw_tester.py`](scripts/bridge_raw_tester.py), [`docs/bridge-raw-command-matrix.example.json`](docs/bridge-raw-command-matrix.example.json), [`README.md`](README.md)): Added a no-reflash bridge command harness with retry/cooldown controls, matrix input format, and structured JSON result output for repeatable raw-command experiments.
- **Oracle A/B command workflow** ([`docs/oracle-ab-playbook.md`](docs/oracle-ab-playbook.md), [`docs/bridge-raw-oracle-matrix.example.json`](docs/bridge-raw-oracle-matrix.example.json), [`scripts/bridge_raw_compare.py`](scripts/bridge_raw_compare.py)): Added a byte-level validation workflow that compares known-good oracle runs against firmware bridge runs to isolate controller-specific write semantics and timing deltas.
- **Source-derived oracle bootstrap artifacts** ([`docs/bridge-raw-oracle-derived-matrix.json`](docs/bridge-raw-oracle-derived-matrix.json), [`docs/bridge-raw-oracle-derived-live-run.json`](docs/bridge-raw-oracle-derived-live-run.json), [`docs/bridge-raw-oracle-derived-diff-vs-live-frame.json`](docs/bridge-raw-oracle-derived-diff-vs-live-frame.json), [`docs/command-write-debug-log.md`](docs/command-write-debug-log.md)): Added code-derived command vectors from upstream `balboa_worldwide_app` and first live comparison artifacts to ground v1 write diagnostics without external packet captures.
- **State-precondition and cadence test matrices** ([`docs/bridge-raw-state-precondition-matrix.json`](docs/bridge-raw-state-precondition-matrix.json), [`docs/bridge-raw-cts-cadence-matrix.json`](docs/bridge-raw-cts-cadence-matrix.json), [`docs/command-write-debug-log.md`](docs/command-write-debug-log.md)): Added focused bridge harness test sets to validate controller-state prerequisites (temp range/heating mode sequencing) and short repeated-toggle timing cadence (100 ms spacing) based on cross-project findings.
- **Live diagnostic run artifacts for new matrices** ([`docs/bridge-raw-state-precondition-live-run.json`](docs/bridge-raw-state-precondition-live-run.json), [`docs/bridge-raw-state-precondition-diff-vs-baseline.json`](docs/bridge-raw-state-precondition-diff-vs-baseline.json), [`docs/bridge-raw-cts-cadence-live-run.json`](docs/bridge-raw-cts-cadence-live-run.json), [`docs/bridge-raw-cts-cadence-diff-vs-baseline.json`](docs/bridge-raw-cts-cadence-diff-vs-baseline.json)): Added first live results and baseline comparisons; transport remained healthy but no setpoint convergence was observed in these variants.
- **Light1 deep diagnostic matrix and live artifacts** ([`docs/bridge-raw-light1-deep-matrix.json`](docs/bridge-raw-light1-deep-matrix.json), [`docs/bridge-raw-light1-deep-live-run.json`](docs/bridge-raw-light1-deep-live-run.json), [`docs/bridge-raw-light1-deep-diff-vs-oracle-derived.json`](docs/bridge-raw-light1-deep-diff-vs-oracle-derived.json), [`docs/command-write-debug-log.md`](docs/command-write-debug-log.md)): Added a Light1-only campaign (cadence, pre-toggle state sequencing, and destination/channel variants). All cases were transport-accepted but no Light1 state transition was observed.
- **Light1 session/context diagnostics** ([`docs/bridge-raw-light1-session-context-matrix.json`](docs/bridge-raw-light1-session-context-matrix.json), [`docs/bridge-raw-light1-session-context-live-run.json`](docs/bridge-raw-light1-session-context-live-run.json), [`docs/command-write-debug-log.md`](docs/command-write-debug-log.md)): Added client-housekeeping/session-context probes (`bf 03`, `bf 05`, `bf 07`) around Light1 toggles to test ownership effects; transport remained healthy but Light1 state remained unchanged.
- **CTS-edge Light1 diagnostic path** ([`lib/localRS485Communication/rs485.cpp`](lib/localRS485Communication/rs485.cpp), [`lib/localRS485Communication/rs485.h`](lib/localRS485Communication/rs485.h), [`lib/spaMessage/spaCommandDispatcher.cpp`](lib/spaMessage/spaCommandDispatcher.cpp), [`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp), [`docs/command-write-debug-log.md`](docs/command-write-debug-log.md)): Added next-CTS frame arming telemetry/counters and `GET /api/diag/light1_next_cts` to A/B test strict "send on immediate ready edge" behavior for Light1 toggles.
- **CTS-edge Light1 live trial artifact** ([`docs/diag-light1-next-cts-live-run.json`](docs/diag-light1-next-cts-live-run.json), [`docs/command-write-debug-log.md`](docs/command-write-debug-log.md)): Added post-OTA endpoint trial results; all trials armed and fired on next CTS successfully, but no Light1 state transition was observed.

### Changed

- **Single-speed pump normalization for controls/status** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Status/control rendering and control-state matching now normalize configured 1-speed pumps to binary On/Off semantics (`pumpNOn`) while preserving raw pump values in diagnostics, reducing false multi-speed assumptions on single-speed spa setups.
- **Bridge-to-RS485 observability** ([`lib/bridge/bridge.cpp`](lib/bridge/bridge.cpp), [`lib/spaMessage/cacheRead.cpp`](lib/spaMessage/cacheRead.cpp), [`lib/spaMessage/spaMessage.cpp`](lib/spaMessage/spaMessage.cpp), [`lib/localRS485Communication/rs485.cpp`](lib/localRS485Communication/rs485.cpp)): Added `[BridgeDiag]` logs for ingress ids, forwarding decisions, queue depth at enqueue/dequeue, and RS485 send timing to correlate raw bridge injections with on-wire transmission.
- **Oracle comparer matching flexibility** ([`scripts/bridge_raw_compare.py`](scripts/bridge_raw_compare.py)): Added `--match-by frame` mode so comparisons can align semantically equivalent runs even when labels differ across matrices.

## [1.6.1] - 2026-04-28

### Changed

- **`/status` run-time readability** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Heater and filter run-time totals on the spa status page now render as **hours and minutes** (for example `2h 05m`) instead of raw seconds.
- **`src/main.h`:** Firmware **`VERSION`** set to **1.6.1**.
- **`lib/Analytics/Analytics.h`:** **`ANALYTICS_VERSION`** aligned with **`VERSION`** (**1.6.1**).

## [1.6.0] - 2026-04-28

### Added

- **Shared command dispatcher API** ([`lib/spaMessage/spaCommandDispatcher.h`](lib/spaMessage/spaCommandDispatcher.h), [`lib/spaMessage/spaCommandDispatcher.cpp`](lib/spaMessage/spaCommandDispatcher.cpp)): Added a reusable spa command layer for v1 write actions with readiness checks and validation. New callable APIs: `spaSendToggleCommand(...)` and `spaSetTargetTemperature(...)`; each builds framed Balboa writes and enqueues via existing `sendMessageToSpa(...)`.

### Changed

- **Command write scope lock** ([`README.md`](README.md), [`AGENTS.md`](AGENTS.md)): Defined implementation scope for spa command writes to reduce protocol risk: **v1** includes only Balboa **`0x11`** toggle/button and **`0x20`** set-temperature paths (web + MQTT). `SystemTime`, `TimeFormat`, and `TempUnits` are explicitly deferred until after v1 hardening.
- **Web SCI command dispatch (v1)** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): `device_request` now dispatches `target_name="Button"` and `target_name="SetTemp"` through the shared command layer, returning explicit accepted/rejected XML responses (including rejection reasons) instead of logging-only behavior.
- **Toggle command frame fix** ([`lib/spaMessage/spaCommandDispatcher.cpp`](lib/spaMessage/spaCommandDispatcher.cpp)): Balboa `0x11` toggle writes now include the required second payload byte (`II 00`) per protocol, fixing no-op button toggles on spa controls like `Light1`.
- **Web button dispatch diagnostics + state-aware toggles** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Added verbose logs for parsed `Button` requests (raw payload, item code, desired state, toggle count) and state-aware toggle count handling for pumps/lights. Pump requests now account for two-speed off transitions (`Low -> High -> Off`) when the web payload asks for `off`.
- **`/status` interactive controls** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Equipment cards on the status page now include action buttons that post SCI `Button` commands to `/devices/sci`, plus a Set Temp control that sends `SetTemp`. Controls display immediate accepted/rejected response text and refresh status after command attempts.
- **`/status` chart rendering hardening** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Improved history chart stability by capping canvas DPR/size, debouncing resize/orientation redraw with `requestAnimationFrame`, and adding context-loss guards to reduce intermittent browser sad-face/crash behavior while scrolling.
- **`/status` command verification feedback** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Added `GET /api/status/controls` and updated status-page command actions to poll for real state transitions after command acceptance, so the UI now distinguishes "accepted" from "accepted and reflected in spa state."
- **`src/main.h`:** Firmware **`VERSION`** set to **1.6.0**.
- **`lib/Analytics/Analytics.h`:** **`ANALYTICS_VERSION`** aligned with **`VERSION`** (**1.6.0**).

## [1.5.0] - 2026-04-28

### Changed

- **Home Assistant MQTT discovery semantics** ([`lib/mqttModule/haMqttDiscovery.cpp`](lib/mqttModule/haMqttDiscovery.cpp), [`lib/spaMessage/spaMqttMessage.cpp`](lib/spaMessage/spaMqttMessage.cpp)): `heating_state`, `spa_state`, `init_mode`, `heating_mode`, `filter_mode`, and `temp_range` are now discovered as enum sensors with explicit options and published as human-readable text (instead of measurement-like numeric semantics).
- **Home Assistant lock/load entities** ([`lib/mqttModule/haMqttDiscovery.cpp`](lib/mqttModule/haMqttDiscovery.cpp), [`lib/spaMessage/spaMqttMessage.cpp`](lib/spaMessage/spaMqttMessage.cpp)): `panel_locked` and `settings_lock` now publish as binary sensors (`Locked`/`Unlocked` payloads), `mister` is now a binary sensor, and stale retained configs from prior sensor types are retracted.
- **Home Assistant clock entity:** `spa_time` discovery was removed so HA no longer creates a rapidly changing clock entity/history stream.
- **Home Assistant device link:** MQTT discovery now sets `device.configuration_url` to `http://<gatewayName>.local/status` so the HA device page can open the ESP web status endpoint.
- **README MQTT/HA docs** ([`README.md`](README.md)): Added an entity-type matrix, documented clock removal, and described the HA web-link/mDNS behavior.
- **`src/main.h`:** Firmware **`VERSION`** set to **1.5.0**.
- **`lib/Analytics/Analytics.h`:** **`ANALYTICS_VERSION`** aligned with **`VERSION`** (**1.5.0**).

## [1.4.3] - 2026-04-27

### Fixed

- **Home Assistant MQTT discovery** ([`lib/mqttModule/haMqttDiscovery.cpp`](lib/mqttModule/haMqttDiscovery.cpp)): Retract optional discovery for **every** equipment slot not in the current desired mask (not only slots published earlier this boot), so **stale retained** `homeassistant/.../config` messages from older firmware or prior broker state are cleared when MQTT connects or when spa configuration is refreshed.

### Changed

- **Home Assistant MQTT discovery icons** ([`lib/mqttModule/haMqttDiscovery.cpp`](lib/mqttModule/haMqttDiscovery.cpp)): Added explicit MQTT discovery `icon` fields for Balboa-like entities (`pump1`-`pump6`, `circ`, `temp_range`, `spa_state`, `filter_mode`) to better match the official Home Assistant Balboa integration visual style.
- **`src/main.h`:** Firmware **`VERSION`** **1.4.3**.
- **`lib/Analytics/Analytics.h`:** **`ANALYTICS_VERSION`** **1.4.3**.

## [1.4.2] - 2026-04-27

### Changed

- **Web portal ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)):** **`/config`** and **`/state`** show **local date/time** for spa **`lastUpdate`** / **`lastRequest`** and ESP wall-clock fields, with optional **Raw epoch (Unix s)** details (same pattern as **`/status`**). **Filter Configuration** adds short scheduling help, **`lastUpdate (filter settings)`**, and **`Filter 2 enabled`** when filter data has been received.
- **`src/main.h`:** Firmware **`VERSION`** **1.4.2**.
- **`lib/Analytics/Analytics.h`:** **`ANALYTICS_VERSION`** **1.4.2**.

## [1.4.1] - 2026-04-27

### Changed

- **README:** Restructured for a self-contained top (lineage, features, web/MQTT/HA, build, M5, OTA, compiler flags, credits) and a **collapsed verbatim snapshot** of [NorthernMan54/esp32_balboa_spa](https://github.com/NorthernMan54/esp32_balboa_spa) branch **`ESP32`** README at the bottom; [FORK.md](FORK.md) lineage updated to name NorthernMan54.
- **Home Assistant MQTT discovery:** On connect, publishes a **minimal** retained set (temperatures, heating state, spa modes, locks, clock, temp scale). **Pumps, loads, mister, and diagnostic model/software** discovery is published only after **`spaConfigurationData`** / **`spaInformationData`** are populated (same install rules as the web `/status` page); slots that drop out of the desired set are **retracted** (empty retained config). Triggered from configuration and information parses in [`lib/spaMessage/spaMessage.cpp`](lib/spaMessage/spaMessage.cpp); logic in [`lib/mqttModule/haMqttDiscovery.cpp`](lib/mqttModule/haMqttDiscovery.cpp).
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

[Unreleased]: https://github.com/shomanjk/esp32_balboa_spa/compare/v1.6.1...HEAD
[1.6.1]: https://github.com/shomanjk/esp32_balboa_spa/compare/v1.6.0...v1.6.1
[1.6.0]: https://github.com/shomanjk/esp32_balboa_spa/compare/v1.5.0...v1.6.0
[1.5.0]: https://github.com/shomanjk/esp32_balboa_spa/compare/v1.4.3...v1.5.0
[1.4.3]: https://github.com/shomanjk/esp32_balboa_spa/compare/v1.4.2...v1.4.3
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
