# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
where version numbers are used.

## [Unreleased]

## [2.26.2] - 2026-08-23

### Added

- **HTTP liveness watchdog + portal page gate** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp), [`src/config-example.h`](src/config-example.h)): Periodic loopback **GET `/api/version`** probes; restart after **3** consecutive failures (`HTTP liveness watchdog` — recovers connected-but-unreachable stacks without rebooting idle gateways). **`/status`**, **`/config`**, and **`/state`** allow only **one** large portal assembly at a time; concurrent requests get **503** `portal page busy`. Optional **`HTTP_LIVENESS_*`** overrides in `config.h`. **`DIAG_FAULT_CAPTURE`** logs heap snapshot before liveness restart.

### Fixed

- **HTTP liveness semantics** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Replace idle-since-last-request restarts with failed loopback probes so a one-off portal visit does not schedule a firmware reboot.

## [2.26.1] - 2026-08-06

### Added

- **Spa Website → firmware portal navigation** ([`balboa-spa`](balboa-spa) submodule): compact **Gateway** menu on the Vue SPA (login + top bar) links to `/status`, `/config`, `/state`, and `/logs`. PWA service worker denylist so those routes are not intercepted as SPA navigations. Requires LittleFS **`uploadfs`** after submodule update (firmware OTA alone does not ship SPA assets).

### Fixed

- **/status, /config, and /state HTML truncation on Atom Lite** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Large portal pages were assembled into one Arduino `String` and could stop growing under heap fragmentation (no PSRAM), leaving mid-document corruption. Those handlers now assemble into small **RAM slabs** (`PortalHtmlChunks`, 4 KiB) and stream with the existing ETag callback path — **no LittleFS spool** (TempHist write panic remains parked). Failed slab alloc returns HTTP **503** instead of truncated HTML. Delivery uses a POD slot-index filler (libstdc++ `std::function` SBO), plus max-alloc headroom and `nothrow` `AsyncCallbackResponse` / **304** so low-heap send also **503**s instead of aborting under `-fno-exceptions` (ETag hits clear slabs before the 304 alloc). `/logs` still uses `String` pending size bench.

### Version bump

- Firmware **`VERSION`** is **`2.26.1`** ([`src/main.h`](src/main.h)); **`ANALYTICS_VERSION`** aligned ([`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.26.0] - 2026-07-29

### Added

- **RS485 OTA-safe boot** ([`lib/localRS485Communication/rs485.cpp`](lib/localRS485Communication/rs485.cpp), [`src/main.ino`](src/main.ino)): Defer `Serial2.begin` until Wi‑Fi is up and `ArduinoOTA.begin()` has completed (no fixed grace delay). After repeated WDT/panic boots that follow a UART begin attempt, enter **RS485 safe mode** (skip UART) so the portal/OTA stay reachable. `POST /api/rs485/retry` clears safe mode and schedules begin on the main loop. Atom Lite (`M5_STATUS_LED_PIN == 27`) refuses GPIO **16/17** (PICO flash pins). Status LED **green/orange**: **fast** alternate in safe mode (only while Wi‑Fi is up), **slow** when UART is up but no valid frames **this boot** (after 15 s grace; uptime stamp kept after the 60 s streak-clear so the slow blink does not drop to solid green). `/api/rs485` and `/api/diagnostics` expose `uartBegun`, `rs485SafeMode`, streak, and related fields. CircularBuffer `sendMessageToSpa` / dispatcher `queueFrame` return **not ready** when UART is deferred or in safe mode (no false accepted). Maintainer notes updated in [`AGENTS.md`](AGENTS.md).

### Version bump

- Firmware **`VERSION`** is **`2.26.0`** ([`src/main.h`](src/main.h)); **`ANALYTICS_VERSION`** aligned ([`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.25.0] - 2026-07-29

### Changed

- **Unify M5 status LEDs on FastLED** ([`src/led_control.cpp`](src/led_control.cpp), [`platformio.ini`](platformio.ini)): Atom Lite (GPIO **27**) and AtomS3 Lite (GPIO **35**) share **`M5_STATUS_LED`** / **`M5_STATUS_LED_PIN`**. Dropped **`M5_ATOM_LED`**, **`M5_ATOMS3_LITE_LED`**, and the **`m5stack/M5Atom`** dependency. Same green/red/blue/yellow meanings. Desk Atom Lite color-verified (red → green). Notes: [`docs/led-fastled-unify.md`](docs/led-fastled-unify.md).

### Version bump

- Firmware **`VERSION`** is **`2.25.0`** ([`src/main.h`](src/main.h)); **`ANALYTICS_VERSION`** aligned ([`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.24.0] - 2026-07-29

### Added

- **M5 AtomS3 Lite tub envs (`M5AtomS3Lite-tub` / `-ota`)** ([`platformio.ini`](platformio.ini)): AtomS3 **Lite** (not the display AtomS3) + USB CDC or espota, tub-side flags (`LOCAL_CLIENT` / `LOCAL_CONNECT` / `BRIDGE` / `DIAG_FAULT_CAPTURE`), Atomic RS485 pins **RX 5 / TX 6** via `build_flags`, onboard RGB via **`M5_ATOMS3_LITE_LED`** at ship time (later unified as **`M5_STATUS_LED`** / GPIO **35**). Bring-up notes: [`wiki/Hardware-targets.md`](wiki/Hardware-targets.md).

### Changed

- **Env-owned RS485 pins for known M5 stacks:** **`M5AtomLite-tub`** / **`-ota`** set **`TX485_Rx=22`**, **`TX485_Tx=19`**, **`AUTO_TX`** in `build_flags` (AtomS3 Lite: **5 / 6**). Generic envs still use `config.h`. [`src/config-example.h`](src/config-example.h) wraps pin/`AUTO_TX` defines in **`#ifndef`** so env `-D` wins. **Migration:** private `config.h` with unconditional `#define TX485_*` / `AUTO_TX` will redefinition-warn/error on M5 envs until wrapped or removed. **Why it matters:** without the Atom Lite env overrides, `#ifndef` defaults (**16 / 17**) are unsafe on the ESP32-PICO-D4 and can interrupt-WDT crash-loop in `rs485Setup()` before Wi‑Fi/OTA come up.

### Version bump

- Firmware **`VERSION`** is **`2.24.0`** ([`src/main.h`](src/main.h)); **`ANALYTICS_VERSION`** aligned ([`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.23.0] - 2026-07-29

### Added

- **Firmware portal dark mode** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): `/status`, `/config`, `/state`, and `/logs` follow the OS light/dark preference by default. **Auto / Light / Dark** toggle (sun, moon, and half-circle icons) cycles the theme and persists per browser via `localStorage` (`portal-theme`). Desktop: icon-only utility control pinned to the nav bar (not a page link). Dark semantic overrides for heat/equip/range/status chips. Canvas charts (Wi‑Fi RSSI, temperature history) track theme CSS variables.

### Version bump

- Firmware **`VERSION`** is **`2.23.0`** ([`src/main.h`](src/main.h)); **`ANALYTICS_VERSION`** aligned ([`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.22.0] - 2026-07-28

### Added

- **Wi‑Fi mesh STA reliability** ([`lib/wifiModule/`](lib/wifiModule/), [`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp), [`src/config-example.h`](src/config-example.h)): App-owned **async** reconnect (no blocking 10s wait / no every-loop `WiFi.begin` thrash); strongest-AP scan/sort when unlocked; optional compile-time **`WIFI_BSSID`** lock; disconnect reason logs; **`GET /api/wifi`** / `/state` expose AP **BSSID**, **STA MAC**, and optional **`bssidLock`**. OTA/Telnet setup once-guarded; GOT_IP side effects avoid blocking `getLocalTime`. **Do not enable `WIFI_BSSID` on the first field OTA** — confirm `/api/wifi` first.

### Changed

- **Wi‑Fi station bring-up** ([`lib/wifiModule/wifiModule.cpp`](lib/wifiModule/wifiModule.cpp)): `WIFI_STA` mode, `setAutoReconnect(false)`, high TX power after STA start, continuous offline watchdog across retries.

### Version bump

- Firmware **`VERSION`** is **`2.22.0`** ([`src/main.h`](src/main.h)); **`ANALYTICS_VERSION`** aligned ([`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.21.1] - 2026-07-28

### Changed

- **Temperature history flash persist parked** ([`lib/tempHistory/`](lib/tempHistory/), [`src/main.h`](src/main.h)): **`TEMP_HISTORY_FLASH_PERSIST`** defaults to **0** — RTC sampling and the 24h chart/MQTT/ePaper path stay on; LittleFS `/TempHist.bin` load/save is compile-gated off after field panics on the write path. Investigation ledger and resume plan: [`docs/temp-history-littlefs-panic.md`](docs/temp-history-littlefs-panic.md).

### Version bump

- Firmware **`VERSION`** is **`2.21.1`** ([`src/main.h`](src/main.h)); **`ANALYTICS_VERSION`** aligned ([`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.21.0] - 2026-07-28

### Added

- **`GET /api/diagnostics`** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp), [`lib/faultCapture/`](lib/faultCapture/)): Gateway post-mortem JSON — **`deviceUptimeMs`**, RTC **`faultLog`**, **`lastBridgeIngress`**, chip temperature fields, and **`panelClockAutoSync`** — sized independently of firmware identity. Builds the JSON object with a single ArduinoJson **`to<JsonObject>()`** so a second `to<>` cannot clear the fault ring (that was the real cause of “missing `faultLog`” on the old combined `/api/version` path). **`/state`** advanced diagnostics loads the fault ring from this endpoint; API Shortcuts includes the link.
- **Restart reason components on `GET /api/version`**: **`espResetReason`** and **`lastRestartIntent`** alongside composite **`restartReason`**.

### Changed

- **`GET /api/version` slimmed** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Identity / update-check metadata only (`version`, `build`, `hostname`, `ip`, restart fields, repo URLs). **`faultLog`**, chip temp, and **`panelClockAutoSync`** moved to **`/api/diagnostics`** so ArduinoJson pool pressure can no longer drop diagnostics or firmware identity keys from a shared document.
- **Restart soft-label attribution** ([`lib/restartReason/`](lib/restartReason/)): Composite **`restartReason`** appends the RTC soft intent (e.g. **OTA Update**) only for **`ESP_RST_SW`**. Panic / WDT / brownout show the ESP reason alone and clear a stale soft label so weeks-old OTA text no longer appears on later crashes.

### Fixed

- **Hourly panic from config refresh overflow** ([`lib/spaMessage/spaMessage.cpp`](lib/spaMessage/spaMessage.cpp)): **`configurationRequest()`** no longer concatenates up to six 10-byte settings frames into one **`SpaWriteQueueMessage`** (`BALBOA_MESSAGE_SIZE` 50). When all datasets were stale after **`STALE_TIME`** (1 hour), `offset` reached 60 and overflowed the message buffer → heap corruption → `ESP_RST_PANIC` on a ~hourly cadence. Each request is now queued separately; **`sendMessageToSpa`** rejects lengths above **`BALBOA_MESSAGE_SIZE`**.

### Version bump

- Firmware **`VERSION`** is **`2.21.0`** ([`src/main.h`](src/main.h)); **`ANALYTICS_VERSION`** aligned ([`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.20.1] - 2026-07-08

### Added

- **ESP State firmware source link** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp), [`src/main.h`](src/main.h)): Firmware links and update-check messages include **Source (`ESP32`)** via **`FIRMWARE_REPO_BRANCH_URL`** (alongside README / Releases). **`GET /api/version`** exposes **`branchUrl`**.

### Changed

- **Firmware update check wording** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Status text treats GitHub Releases and the default branch as tagged vs tip **source** snapshots (changelog / tag / build from tip), with in-message links instead of download-oriented “open Releases” copy.
- **ESP State firmware repo links** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Dropped the **Firmware repo:** label so **README · Releases · Source (ESP32)** stay on one row.

### Version bump

- Firmware **`VERSION`** is **`2.20.1`** ([`src/main.h`](src/main.h)); **`ANALYTICS_VERSION`** aligned ([`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.20.0] - 2026-07-06

### Added

- **ESP State dual firmware version check** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp), [`src/main.h`](src/main.h)): **Check for updates** on `/state` now compares this gateway against both the latest GitHub **release** and the `VERSION` in `src/main.h` on **`FIRMWARE_REPO_DEFAULT_BRANCH`** (default **`ESP32`**). Three-column pills: **This gateway**, **Latest release**, and **branch**; status text distinguishes release vs pre-release (build-from-source) updates. **`GET /api/version`** adds **`defaultBranch`** and **`mainHContentsApiUrl`**.

### Changed

- **`FIRMWARE_REPO_README_URL`** ([`src/main.h`](src/main.h)): README link now uses **`FIRMWARE_REPO_DEFAULT_BRANCH`** instead of hard-coded **`main`**.

### Version bump

- Firmware **`VERSION`** is **`2.20.0`** ([`src/main.h`](src/main.h)); **`ANALYTICS_VERSION`** aligned ([`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.19.1] - 2026-07-06

### Fixed

- **`GET /api/version` JSON truncation** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): With **`DIAG_FAULT_CAPTURE`**, a full **`faultLog`** ring plus newer fields (`chipTemp*`, **`panelClockAutoSync`**) could exhaust the **5120**-byte ArduinoJson pool so only the last key survived. Pool raised to **10240**; **`faultLog`** / diagnostics appended first, **`version`** / **`build`** / **`hostname`** and related metadata appended **last** so eviction cannot drop firmware identity fields.

### Version bump

- Firmware **`VERSION`** is **`2.19.1`** ([`src/main.h`](src/main.h)); **`ANALYTICS_VERSION`** aligned ([`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.19.0] - 2026-07-06

### Added

- **Panel clock auto-sync on boot** ([`lib/spaMessage/spaCommandDispatcher.cpp`](lib/spaMessage/spaCommandDispatcher.cpp), [`lib/spaMessage/spaMessage.cpp`](lib/spaMessage/spaMessage.cpp), [`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Tub builds (`LOCAL_CLIENT`) may set the spa panel clock once per boot via Balboa **`0x21`** when gateway NTP time and panel time differ by more than **`AUTO_SYNC_PANEL_CLOCK_THRESHOLD_MIN`** (default 2 minutes), after a 45s post-status delay. Compile-time flag **`AUTO_SYNC_PANEL_CLOCK`** defaults to **off** when omitted from existing **`config.h`**; new installs copying [`src/config-example.h`](src/config-example.h) default **on**. **`GET /api/version`** exposes **`panelClockAutoSync`**; **`/status`** panel clock section notes the setting.

### Version bump

- Firmware **`VERSION`** is **`2.19.0`** ([`src/main.h`](src/main.h)); **`ANALYTICS_VERSION`** aligned ([`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.18.6] - 2026-07-06

### Added

- **Gateway chip temperature (portal only)** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): **`GET /api/version`** exposes ESP32 die **`chipTempC`** with **`chipTempStatus`** / **`chipTempStatusLabel`** (Normal / Elevated / High / Critical). **`/state`** → **Show advanced diagnostics** → **Chip temperature** sub-card below Memory (badge + °C). No MQTT or HA discovery.
- **Gateway WiFi RSSI (MQTT + HA)** ([`lib/mqttModule/mqttModule.cpp`](lib/mqttModule/mqttModule.cpp), [`lib/mqttModule/haMqttDiscovery.cpp`](lib/mqttModule/haMqttDiscovery.cpp)): Periodic **`Spa/<gateway>/node/rssi`** telemetry (dBm) when Wi‑Fi is connected; HA MQTT discovery diagnostic **`Gateway WiFi signal`** (`signal_strength`, **`enabled_by_default`: false** — enable on the **Balboa Spa** device page under **Diagnostic** when wanted).

### Changed

- **`/status` panel reminder hints** ([`lib/spaMessage/balboa.h`](lib/spaMessage/balboa.h), [`lib/spaMessage/spaMessage.cpp`](lib/spaMessage/spaMessage.cpp), [`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Banner subtext is per reminder type (e.g. **Change Water** ~90 days, **Clean Filter** ~30 days, sanitizer/pH ~7 days) instead of a single “every 7 days” line. **`GET /api/status/controls`** adds **`reminderHint`** for live polling.
- **PlatformIO local ports:** Device-specific `upload_port` / `monitor_port` values moved out of committed [`platformio.ini`](platformio.ini) into gitignored **`platformio_local.ini`** (template: [`platformio_local.ini.example`](platformio_local.ini.example)).
- **Live diagnostic captures:** Removed maintainer-specific `docs/diag-*.json` and `docs/telnet-*.txt` from the repo (gitignored); redacted hostnames in [`docs/command-write-debug-log.md`](docs/command-write-debug-log.md); added [`docs/diag-light1-next-cts-live-run.example.json`](docs/diag-light1-next-cts-live-run.example.json) as a redacted API shape reference.
- **Privacy / local-only tooling:** [`emulator/bridgeClient.js`](emulator/bridgeClient.js) no longer hardcodes a tub hostname (use `SPA_BRIDGE_HOST` or CLI arg). Removed tracked [`.clang_complete`](.clang_complete), [`.gcc-flags.json`](.gcc-flags.json) (upstream IDE paths), and [`docs/bridge-raw-last-run.json`](docs/bridge-raw-last-run.json) (already gitignored generated output).

### Version bump

- Firmware **`VERSION`** is **`2.18.6`** ([`src/main.h`](src/main.h)); **`ANALYTICS_VERSION`** aligned ([`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.18.5] - 2026-07-06

### Fixed

- **Reminder text — Change Water** ([`lib/spaMessage/balboa.h`](lib/spaMessage/balboa.h)): Map status byte 6 value **`0x08`** to **Change Water** (verified on **BP501** / **CL501X1** with topside **CHNG WATR**). **`/status`**, MQTT **`status/reminderText`**, and HA **`reminder`** sensor no longer show the generic **Maintenance reminder** for that code.

### Version bump

- Firmware **`VERSION`** is **`2.18.5`** ([`src/main.h`](src/main.h)); **`ANALYTICS_VERSION`** aligned ([`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.18.4] - 2026-06-29

### Fixed

- **MQTT/HA blower status** ([`lib/spaMessage/spaMqttMessage.cpp`](lib/spaMessage/spaMqttMessage.cpp), [`lib/spaMessage/spaMessage.cpp`](lib/spaMessage/spaMessage.cpp)): Publish **`On`/`Off`** when the status blower field is non-zero; fixes **`Unknown (0x03)`** feedback when controllers report values other than `1` ([issue #10](https://github.com/shomanjk/esp32_balboa_spa/issues/10)). Web **`/status`** blower label uses the same rule on first render.

### Version bump

- Firmware **`VERSION`** is **`2.18.4`** ([`src/main.h`](src/main.h)); **`ANALYTICS_VERSION`** aligned ([`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.18.3] - 2026-06-23

### Changed

- **Fault log history table** ([`lib/spaWebServer/spaConfigExport.cpp`](lib/spaWebServer/spaConfigExport.cpp), [`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): **`GET /api/config/fault-log/history`** includes partial **`entries`** while a scan is **`loading`**; **`/config`** poll re-renders the table every ~2s so rows appear as slots are read.

### Version bump

- Firmware **`VERSION`** is **`2.18.3`** ([`src/main.h`](src/main.h)); **`ANALYTICS_VERSION`** aligned ([`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.18.2] - 2026-06-23

### Fixed

- **Fault log history RS485 reliability** ([`lib/spaMessage/spaMessage.cpp`](lib/spaMessage/spaMessage.cpp)): Pause all **`configurationRequest()`** / filter readback traffic during a history scan so queued fault-log reads are not starved; ignore fault-log responses for the wrong entry index; retry each entry up to 3 times (30s apart) before skipping to the next slot; seed **`targetCount`** from the known latest log size at scan start. History API adds **`pendingEntry`** for accurate progress.

### Version bump

- Firmware **`VERSION`** is **`2.18.2`** ([`src/main.h`](src/main.h)).

## [2.18.1] - 2026-06-23

### Fixed

- **Fault log history load stuck on “Starting history load…”** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Register **`/api/config/fault-log/history`** before **`/api/config/fault-log`** so ESPAsyncWebServer does not serve latest-event JSON for history polls; portal JS validates the history response shape and keeps polling.

- **Fault log history scan** ([`lib/spaMessage/spaMessage.cpp`](lib/spaMessage/spaMessage.cpp)): Skip automatic fault-log refresh in **`configurationRequest()`** while a history scan is active; allow restarting a scan after per-entry timeout if the previous run stalled.

### Version bump

- Firmware **`VERSION`** is **`2.18.1`** ([`src/main.h`](src/main.h)).

## [2.18.0] - 2026-06-23

### Added

- **Fault log full history** ([`lib/spaMessage/spaMessage.cpp`](lib/spaMessage/spaMessage.cpp), [`lib/spaWebServer/spaConfigExport.cpp`](lib/spaWebServer/spaConfigExport.cpp), [`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): On-demand RS485 scan of all spa fault-log slots (up to 24 entries) via **`POST /api/config/fault-log/history`** and poll **`GET /api/config/fault-log/history`**; expandable **View full event history** table on **`/config`**. Latest event stays stable during scan; **`0xFF`** refresh when complete.

- **Fault log API** ([`lib/spaWebServer/spaConfigExport.cpp`](lib/spaWebServer/spaConfigExport.cpp)): **`GET /api/config/fault-log`** returns latest event JSON with **`eventText`**, **`severity`**, and panel-clock **`occurredText`**.

### Changed

- **`/config` spa controller history** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Renamed from **Other spa datasets**; **Latest event** card with decoded headline, severity badge, and intro distinguishing spa log vs live **`/status`** vs ESP **`GET /api/version` → `faultLog`**. Settings **`0x04`** moved under **Developer: undecoded settings (0x04)**.

### Fixed

- **Blank fault log message after reboot** ([`lib/spaMessage/spaMessage.cpp`](lib/spaMessage/spaMessage.cpp), [`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Recompute **`faultMessage`** at boot when RTC retains code but not the string; portal and export use **`spaFaultMessageForCode()`** at render time instead of stale persisted text.

### Version bump

- Firmware **`VERSION`** is **`2.18.0`** ([`src/main.h`](src/main.h)).

## [2.17.1] - 2026-06-23

### Changed

- **`/config` equipment wiring** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Drop redundant “Configured on this pack” summary; shorten intro to link to **Spa Status** only.

- **`/status` equipment** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): **Show not installed** checkbox (off by default) hides greyed-out equipment cards; check to show the full grid including absent slots.

## [2.17.0] - 2026-06-23

### Added

- **Panel preferences fetch and reminders control** ([`lib/spaMessage/spaMessage.cpp`](lib/spaMessage/spaMessage.cpp), [`lib/spaMessage/spaCommandDispatcher.cpp`](lib/spaMessage/spaCommandDispatcher.cpp), [`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Gateway requests Balboa **Preferences** (`0x22` / `0x08`); **`/config`** → **Panel preferences** shows maintenance **Reminders** on/off with browser confirm; **`GET/POST /api/config/preferences`** (`0x27` write). Polls until preferences arrive when not yet received. Some packs (e.g. **M100** / **CL501X1**) encode reminders as a flag byte (**bit 0** = on); **`0x85`** displays as **On** instead of **Unknown (133)**.

- **`/status` reminder visibility** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp), [`lib/spaMessage/spaMessage.cpp`](lib/spaMessage/spaMessage.cpp)): Top **Panel reminder** banner when a maintenance reminder is active; amber for routine reminders (Clean Filter, pH, etc.), red for fault-class (`0x1E`). Detail row in **Panel and flags** uses matching amber/red styling. API adds `reminderActive` and `reminderIsFault`. Suppress false reminders during priming (`initMode == 1`) as well as spa initializing.

### Changed

- **Reminder text** ([`lib/spaMessage/spaMessage.cpp`](lib/spaMessage/spaMessage.cpp)): Unmapped non-zero reminder codes show **Maintenance reminder** instead of `Unknown (0xNN)`.

- **`/config` equipment wiring** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Human-readable **Configured as** labels (Not installed, 1-speed, 2-speed, Installed), summary line of fitted loads, muted rows for absent slots, clearer intro vs live **Spa Status**. Developer fields (magic number, CRC, raw hex) moved under **Configuration metadata & raw frame**; removed misleading **`temp_scale`** row (not decoded from the configuration frame).

## [2.16.0] - 2026-06-23

### Fixed

- **Portal pages missing global CSS** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Build shared `<head>` with sequential `String` appends (`appendPortalHead`) instead of chained `String` temporaries; portal nav/`ePaper` snippets use string literals. If `:root` portal CSS is absent before send, log an error only (no full-page repair copy that could exhaust heap on `/status`). `/status` HTML reserve raised to **70000**.

### Changed

- **ESP State intro** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Note that **Reboot gateway** and deeper diagnostics live under **Show advanced diagnostics**.

## [2.15.0] - 2026-06-23

### Added

- **Web gateway reboot** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): **ESP State** (`/state`) advanced **Gateway Actions** card with a confirmed **Reboot gateway** button (`GET /restart`); manual restarts record **`Web restart`** in restart reason.

### Changed

- **ESP State layout** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): **Free Heap** and RS485 counters (**mode**, **valid frames**, **CRC errors**, **last frame age**) move behind **Show advanced diagnostics**; **RS485 Health** stays in the default view.
- **ESP State WiFi panel** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Compact summary strip (status badge, SSID/hostname, RSSI), meta line (channel/MAC/status code), and two-column **Network** + **Signal** layout with live RSSI chart (same fields and `GET /api/wifi` polling as before).
- **ESP State System Health** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Hero strip (uptime, clock, RS485 badge), restart-reason meta line, and advanced **Memory** / **RS485 today** / **Build** / **Gateway Actions** sub-card grid; **Advanced Diagnostics** RS485 counters use a today-vs-yesterday comparison table.

### Version bump

- Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are **`2.15.0`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.14.1] - 2026-06-23

### Fixed

- **spaMessage parse buffers** ([`lib/spaMessage/balboa.h`](lib/spaMessage/balboa.h), [`lib/spaMessage/spaMessage.cpp`](lib/spaMessage/spaMessage.cpp)): Enlarge WiFi module **`macAddress`** to 18 bytes (fits `aa:bb:cc:dd:ee:ff` + null); use **`snprintf`** for MAC, software ID, model, and dip-switch fields parsed from RS485 information / WiFi-config responses.

### Changed

- **Documentation sync** ([`README.md`](README.md), [`AGENTS.md`](AGENTS.md), [`esp32_robustness_hardening_revised.plan.md`](esp32_robustness_hardening_revised.plan.md)): Align command-write scope with shipped behavior — MQTT **`cmd/#`** dispatch and web SCI paths are implemented; **`TimeFormat`** is web + config export/import (not deferred); note remaining gap for MQTT **`cmd/timeFormat`**. Robustness plan marks command-path phase complete and updates baseline/gap lists.

### Version bump

- Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are **`2.14.1`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.14.0] - 2026-06-22

### Added

- **Fault log decode and fetch** ([`lib/spaMessage/spaMessage.cpp`](lib/spaMessage/spaMessage.cpp), [`lib/spaMessage/balboa.h`](lib/spaMessage/balboa.h)): Proactive **`FAULT_LOG_REQUEST`** (`0x22` / `0x20` / `0xFF`) in **`configurationRequest()`**; **`parseFaultResponse()`** decodes code, message, entry counts, and relative time. MQTT **`faultLog/*`** topics include **`faultLogTime`**; **`/config`** and config export snapshot show decoded fields; HA discovery sensors for fault code, message, and time.
- **Reminder text** ([`lib/spaMessage/balboa.h`](lib/spaMessage/balboa.h), [`lib/spaMessage/spaMqttMessage.cpp`](lib/spaMessage/spaMqttMessage.cpp), [`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): **`reminderTypeMap`** maps status byte 6 to human-readable text; MQTT **`status/reminderText`**; **`/status`** reminder row with alert styling when active; HA diagnostic **`reminder`** sensor.

### Version bump

- Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are **`2.14.0`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.13.0] - 2026-06-22

### Added

- **MQTT filter schedule writes** ([`lib/mqttModule/mqttModule.cpp`](lib/mqttModule/mqttModule.cpp), [`lib/spaMessage/spaCommandDispatcher.cpp`](lib/spaMessage/spaCommandDispatcher.cpp)): **`Spa/<gateway>/cmd/filter`** JSON (same schema as **`POST /api/config/filter`**, merges omitted fields from live cache); granular **`cmd/filter/filter{1,2}/{start,duration}`** with **`HH:MM`** payloads; **`cmd/filter/filter2/enabled`** (`true`/`false`/`on`/`off`/`1`/`0`). Outcomes on **`cmd/result`**.
- **Filter running telemetry** ([`lib/spaMessage/spaMqttMessage.cpp`](lib/spaMessage/spaMqttMessage.cpp), [`lib/mqttModule/haMqttDiscovery.cpp`](lib/mqttModule/haMqttDiscovery.cpp)): **`status/filter1_running`** and **`status/filter2_running`** (`On`/`Off`) derived from live controller **`filterMode`**; HA **`binary_sensor`** discovery; **`GET /api/status/controls`** includes **`filter1_running`** / **`filter2_running`**.

### Changed

- **Shared filter JSON parsing** ([`lib/spaMessage/spaCommandDispatcher.cpp`](lib/spaMessage/spaCommandDispatcher.cpp)): Web **`POST /api/config/filter`**, config import, and MQTT filter commands share **`spaParseFilterCycleJson`** / **`spaApplyFilterGranularMqtt`**.

### Version bump

- Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are **`2.13.0`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.12.4] - 2026-06-10

### Fixed

- **Filter 2 disable save/readback** ([`lib/spaMessage/spaCommandDispatcher.cpp`](lib/spaMessage/spaCommandDispatcher.cpp), [`lib/spaMessage/spaMessage.cpp`](lib/spaMessage/spaMessage.cpp), [`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Normalize disabled Filter 2 schedule fields on POST and RS485 cache; schedule follow-up filter reads ~2s after writes; UI verification compares **`enabled`** only when Filter 2 is off (avoids false “readback mismatch”).
- **`/config` filter start time vs spa panel format** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Start time pickers use `lang` hints (`en-US` / `en-GB`) from live **`clockMode`** so 12h/24h UI aligns with the spa panel (still `type="time"`; values remain 24h wire format).

### Changed

- **`/config` filter UI copy** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Shorten duration labels and filter section caption; bottom-align Filter 1/2 field blocks.

### Version bump

- Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are **`2.12.4`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.12.3] - 2026-06-10

### Fixed

- **`/config` filter save readback** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): After a verified save, update the **Filter 2 enabled** summary row from `GET /api/config/filter` (was stale until full page reload).

### Version bump

- Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are **`2.12.3`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.12.2] - 2026-06-10

### Fixed

- **`/config` filter cards layout** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Start time and duration fields **bottom-align** across Filter 1 and Filter 2 columns (Filter 1 has top padding where Filter 2’s enable checkbox sits; Filter 1 cannot be disabled on the controller).

### Version bump

- Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are **`2.12.2`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.12.1] - 2026-06-10

### Fixed

- **`/config` filter duration fields** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Duration uses **hours + minutes** number inputs instead of a clock-style time picker (no AM/PM on duration). Fixes blank display when the controller reports a **24 h** cycle (`24:00` is invalid for HTML `type="time"`).

### Version bump

- Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are **`2.12.1`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.12.0] - 2026-06-10

### Added

- **Filter schedule writes (Balboa `0x23`)** ([`lib/spaMessage/spaCommandDispatcher.cpp`](lib/spaMessage/spaCommandDispatcher.cpp), [`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Shared **`spaSetFilterCycles()`** with validation, RS485 frame build, and post-write **`spaRequestFilterSettings()`** readback. SCI **`Filters`** read/write routing fixed (writes no longer blocked by the early `"Filters"` catch-all). **`GET`/`POST /api/config/filter`** JSON API; editable filter 1/2 schedule on **`/config`** with save + readback polling.
- **Spa config backup & restore** ([`lib/spaWebServer/spaConfigExport.cpp`](lib/spaWebServer/spaConfigExport.cpp)): **`GET /api/config/export`** downloads writable settings (filter, panel clock, temp units) plus read-only snapshots (information, configuration, preferences, settings `0x04`, fault log). **`POST /api/config/import`** applies writable sections with identity mismatch warnings (model / configuration signature); blocked unless **`force: true`**. **`/config`** Backup & restore panel (download, preview with **`dryRun`**, apply).

### Changed

- **SCI filter compatibility:** **`target_name="Request">Filters`** read and **`target_name="Filters">${base64}`** write match [balboa-spa `balboa.js`](https://github.com/jozefnad/balboa-spa/blob/master/src/assets/balboa.js) envelope bytes `[4..11]`.

### Version bump

- Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are **`2.12.0`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.11.2] - 2026-06-10

### Fixed

- **`/status` temperature range bands (desktop)** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Pin low/high setpoint buttons to **grid row 1** on wide viewports so they stay side by side after the DOM reorder for mobile stacking.

### Version bump

- Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are **`2.11.2`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.11.1] - 2026-06-10

### Changed

- **`/status` temperature range bands** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): High/low setpoint buttons stack **high on top, low below** on narrow viewports; desktop layout unchanged (low left, high right). DOM order matches mobile stack so tab and screen-reader order align with visual layout.

### Version bump

- Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are **`2.11.1`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.11.0] - 2026-06-10

### Fixed

- **Portal `/status` history charts** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Heater and filter series include **today in progress** (same merge as ePaper), with clearer captions, oldest-first raw lists, axis units, and relative X labels (`-24h`/`now`, `-23d`/`today`). **`GET /api/status/histories`** exposes merged `heatSeconds` / `filterSeconds` plus `heatTodaySeconds` / `filterTodaySeconds`.

### Changed

- **Temperature history** ([`lib/tempHistory/`](lib/tempHistory/), [`lib/spaMessage/spaMessage.cpp`](lib/spaMessage/spaMessage.cpp)): **144 samples** at **10-minute** intervals (24h window) in RTC RAM; **`/TempHist.bin`** on LittleFS with hourly persist when the buffer changed. Removed `temperatureHistory[]` from [`SpaStatusData`](lib/spaMessage/balboa.h). Portal/MQTT/ePaper use the new series; **`GET /api/status/histories`** adds `tempSlotCount` / `tempSampleMinutes`.

- **Licensing:** Firmware (`src/`, `lib/` except vendored carve-outs, build tooling, docs) is now **[PolyForm Noncommercial 1.0.0](LICENSE-firmware)** (licensor: **Jerrod Kogut (shomanjk)**); commercial use requires separate permission. The `balboa-spa/` web UI submodule stays **Apache-2.0**. See [`LICENSE`](LICENSE) and [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

### Documentation

- **README:** Add human-facing [**Overview**](README.md#overview) at the top (value, parts, setup expectations, shipped capabilities); rename former **Features** section to **Usage history and analytics**; remove duplicate **What this project is** (developer build roles summarized in Overview).
- **README / wiki [Hardware field notes](https://github.com/shomanjk/esp32_balboa_spa/wiki/Hardware-field-notes):** Document community-tested **Molex `0451320403`** ([DigiKey `WM16117-ND`](https://www.digikey.com/short/p5ctrr0m)) for mating the factory header on a **Balboa BP501** spa board ([`wiki/Hardware-field-notes.md`](wiki/Hardware-field-notes.md)).
- **Wiki [Home](https://github.com/shomanjk/esp32_balboa_spa/wiki):** Link to README Overview ([`wiki/Home.md`](wiki/Home.md)).
- **Wiki:** Use `blob/ESP32/` for in-repo links (`README`, `FORK.md`, `OTA_LOGGING_WORKFLOW.md`, etc.) so URLs match the default branch and avoid **404** when `main` lags ([`wiki/*.md`](wiki/)).

### Version bump

- Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are **`2.11.0`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.10.6] - 2026-06-10

### Fixed

- **Portal `/status` history charts** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Harden lazy-load UX for **Load history charts** and chart-icon links — validate injected HTML before marking loaded, scroll to the chart section after load, show visible errors (`#statusHistoriesResult`) instead of silent no-ops, fix chart-icon handler to always delegate to `statusLoadHistories`, null-safe listener init, and `finally` guard on the loading flag. **`GET /api/status/histories`** null-checks analytics pointers, bumps JSON capacity, and returns explicit errors on overflow/serialize failure.

### Version bump

- Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are **`2.10.6`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.10.5] - 2026-06-10

### Fixed

- **Bridge idle logging** ([`lib/bridge/bridge.cpp`](lib/bridge/bridge.cpp)): Drop per-frame `bridge/out skipped` verbose lines when Homebridge is not connected; log once at **notice** when the **last** TCP client disconnects (`bridge/out idle — no TCP client connected`). Steady-state without any bridge client is silent.

## [2.10.4] - 2026-06-10

### Changed

- **DIAG fault log on `GET /api/version`** ([`lib/faultCapture/faultCapture.cpp`](lib/faultCapture/faultCapture.cpp)): `faultLog` is now an array of objects (`uptimeMs`, `msg`, optional `wallUnix` / `wallTime`) plus `deviceUptimeMs` for same-boot comparison. RTC ring layout bump clears legacy string-only entries on first boot after upgrade.

## [2.10.3] - 2026-06-10

### Fixed

- **Task watchdog (TASK_WDT) panics on tub builds** ([`src/main.ino`](src/main.ino), [`lib/wifiModule/wifiModule.cpp`](lib/wifiModule/wifiModule.cpp), [`lib/localRS485Communication/rs485.cpp`](lib/localRS485Communication/rs485.cpp)): Feed the task WDT every `loop()` iteration and during the Wi‑Fi connect wait so Wi‑Fi outages no longer starve the watchdog while RS485 is still healthy (`spaMessageLoop` only ran when `WL_CONNECTED`, and `esp_task_wdt_reset()` was only called when dequeuing the spa read queue). Replace the implicit “no spa traffic” TWDT behavior with an explicit **SPA silence watchdog** (`rs485CheckSpaSilenceWatchdog`) that restarts cleanly when no valid RS485 frame arrives for `RUNNING_WDT_TIMEOUT` seconds after spa id assignment.

## [2.10.2] - 2026-06-10

### Fixed

- **`GET /api/version` with `DIAG_FAULT_CAPTURE`** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Append `faultLog` / `lastBridgeIngress` before firmware metadata and size the JSON doc to **4096** bytes so a full 16-line fault ring no longer evicts `version`, `build`, `hostname`, and related fields (ArduinoJson drops oldest keys when the pool is exhausted).

## [2.10.1] - 2026-06-10

### Fixed

- **Bridge MQTT noise** ([`lib/bridge/bridge.cpp`](lib/bridge/bridge.cpp)): Stop publishing `Client not connected` on `Spa/<gateway>/bridge/msg` for every RS485 frame when no TCP client is on port **4257** (was ~1–4/s on default tub builds). TCP forwarding and `bridge/out` MQTT when a client **is** connected are unchanged; ingress `bridge/msg` / `bridge/in` behavior is unchanged. Bridge TCP connect/disconnect now both publish once on `Spa/<gateway>/debug/message` (`Bridge Client Connected …` / `Bridge Client Disconnected …`).

## [2.10.0] - 2026-05-15

### Changed

- **Portal `GET /config` layout:** Responsive two-column grid from **720px** (aligned with `/status`), in-page section links, panels ordered **equipment wiring → controller identity → filter configuration → panel preferences → other datasets → LittleFS**, `dl` key/value rows for identity/preferences/metadata, equipment wiring as a **table**, Filter 1/2 summary cards plus concise footer rows, monospace hex in `<pre>` with horizontal scroll, and full-width panels for **Other datasets** and **LittleFS** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)).

### Added

- **`GET /config` spa diagnostics:** Portal Spa Configuration page now shows **controller identity** from the Information response (`0x24`): software ID, **system model**, setup number, configuration signature, heater voltage/type (with protocol-minded labels), DIP switches, CRC; **preferences** block; expanded equipment CRC plus collapsible raw hex for information / preferences / configuration / filter / settings-0x04 / fault frames; **other datasets** (settings `0x04`, fault log) when received ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)). Filter Configuration and LittleFS sections render even when the equipment configuration frame is still missing.

### Documentation

- **Wiki [Hardware field notes](https://github.com/shomanjk/esp32_balboa_spa/wiki/Hardware-field-notes):** Cross-check **BP501** / **CL501X1** / agency strings against [Balboa BP 501/601](https://www.balboawatergroup.com/BP501/) and [ccutrer protocol.md](https://github.com/ccutrer/balboa_worldwide_app/blob/master/doc/protocol.md); add label-reading guide and **BP501-CL501X1-AS** reporter row (source: [`wiki/Hardware-field-notes.md`](wiki/Hardware-field-notes.md)).
- **README:** Under [Build with PlatformIO](README.md#build-with-platformio), document **firmware first, then `uploadfs`** for a first install, note that LittleFS assets are not in the firmware image by default, and link the [Getting started](https://github.com/shomanjk/esp32_balboa_spa/wiki/Getting-started) wiki checklist ([issue #6](https://github.com/shomanjk/esp32_balboa_spa/issues/6)).

### Fixed

- **Submodule setup and Windows `uploadfs` / `buildfs`** ([`scripts/extra_script.py`](scripts/extra_script.py), [`.gitignore`](.gitignore), [issue #6](https://github.com/shomanjk/esp32_balboa_spa/issues/6)): Accidental **`.claude/worktrees/…`** entries had been committed as gitlinks without `.gitmodules` entries, which broke `git submodule update --init --recursive` for fresh clones. **Removed `.claude/` from the entire repository history** (`git filter-repo`) so clones no longer carry that baggage; added **`.claude/`** to `.gitignore`. Replaced **`cp`** with **`shutil.copy2`** when copying `.env` into `balboa-spa/dist/` so LittleFS builds work on Windows without a POSIX `cp`.

**Maintainers:** Pushing this update requires **`git push --force-with-lease`** to **`origin/ESP32`** (and any other rewritten branches). Contributors with old clones should **`git fetch origin`**, **`git reset --hard origin/ESP32`**, or re-clone.

### Version bump

- Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are **`2.10.0`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.9.0] - 2026-05-14

### Fixed

- **Bridge `%02x` panic on Balboa Worldwide connect** ([`lib/spaMessage/cacheRead.cpp`](lib/spaMessage/cacheRead.cpp), [PR #5](https://github.com/shomanjk/esp32_balboa_spa/pull/5)): `ArduinoLog` **1.1.1** does not support printf-style width modifiers — `printFormat` only recognizes single-char specifiers. The `0` in `%02x` was treated as an unknown spec, consumed no `va_arg`, and the trailing `2x` printed as literal text. Subsequent `%u` / `%s` then consumed the wrong va_args: the `[BridgeDiag]: fragment type=0x%02x len=%u frame=%s` line in `processFragment` had `%s` consume the `length` integer (e.g. `0x0a` for a config-request frame) and pass it to `Print::print(const char *)`, which called `strlen(0x0a)` and panicked with **`LoadProhibited` / `EXCVADDR=0x08`** (see [issue #4](https://github.com/shomanjk/esp32_balboa_spa/issues/4)). Replaced **`%02x`** with **`%x`** in the three **`BRIDGE_LOG_NOISY`** call sites in `processFragment`. The earlier v2.8.3 `frameHex` lifetime fix was unrelated. Diag output loses single-digit zero padding (`type=0x4` rather than `type=0x04`).
- **TCP bridge ingress hardening (no accumulator)** ([`lib/spaMessage/cacheRead.cpp`](lib/spaMessage/cacheRead.cpp), [`lib/bridge/bridge.cpp`](lib/bridge/bridge.cpp)): Stateless `cacheRead` with **min/max frame length** checks, **resync** on bogus length bytes, **break** when a chunk ends on a lone `0x7E` (avoids spinning), and **`length <= BALBOA_MESSAGE_SIZE`** plus **`length >= 5`** before reading **`message[4]`** in the Wi‑Fi module fast path — small guards without a **cross-chunk buffer**.
- **Portal `/logs` scroll while tailing** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Live poll/WebSocket updates no longer force the view to the bottom when you have scrolled up to read older lines (only repaints in place when you are already near the bottom; otherwise lines buffer and the **new lines** control applies as before). Full redraws after filters/presets keep scroll position unless you were already pinned to the tail. The redundant **Auto-scroll** checkbox was removed (**Pause** still stops the stream).

### Version bump

- Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are **`2.9.0`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.8.6] - 2026-05-14

### Changed

- **`TELNET_LOG` default** ([`platformio.ini`](platformio.ini)): Tub-side and OTA envs (**`M5AtomLite-tub`**, **`M5AtomLite-tub-ota`**, **`ESP32ota`**, **`ESP32prodOta`**) no longer pass **`-DTELNET_LOG`** by default, so **TelnetStream** is not started (no TCP **23**). The **`#ifdef TELNET_LOG`** implementation in [`lib/wifiModule/wifiModule.cpp`](lib/wifiModule/wifiModule.cpp) / [`src/main.ino`](src/main.ino) is unchanged; re-enable by uncommenting or adding **`-DTELNET_LOG`** to your env. Docs: [`README.md`](README.md), [`AGENTS.md`](AGENTS.md), [`OTA_LOGGING_WORKFLOW.md`](OTA_LOGGING_WORKFLOW.md), portal **`/logs`** intro ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)).
- **Wi‑Fi Telnet boot lines** ([`lib/wifiModule/wifiModule.cpp`](lib/wifiModule/wifiModule.cpp)): When **`TELNET_LOG`** *is* enabled, log text now states that **TelnetStream** is listening on **23** and that the **Serial + web log tee** is unchanged (avoids “switching to telnet” implying **`Log`** moved).

### Version bump

- Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are **`2.8.6`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.8.5] - 2026-05-14

### Fixed

- **Portal `/logs` Pause with WebSocket tail** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Closing the socket for Pause no longer triggers the WS **`onclose`** fallback that unconditionally started HTTP polling, which made the stream appear to keep running. **`onclose`** now starts poll fallback only when not paused; WS **`onmessage`** ignores frames while paused; in-flight **`GET /api/logs`** is aborted on Pause / mode switch / tab hide.

### Changed

- **Portal `/logs` layout** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Logs-only **`html`/`body`** flex column with **`100svh` / `100dvh`**, safe-area padding, and a flex-growing **`#logView`** (`min-height: 0`, **`flex: 1 1 12rem`**) so the viewer uses remaining viewport height on large displays. **`WebSocket tail`** defaults to checked in markup to match the script default.

### Version bump

- Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are **`2.8.5`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.8.4] - 2026-05-14

### Fixed

- **Bridge client IP in logs** ([`lib/bridge/bridge.cpp`](lib/bridge/bridge.cpp)): AsyncTCP teardown and error callbacks often report **`remoteIP()`** as **`0.0.0.0`**. The firmware now caches each slot’s address when the client is assigned and uses that (with fallback) for disconnect/connect/timeout/error logging, **`bridgeSend`** send-fail / **`faultCapture`** lines, and **`[BridgeDiag]`** ingress **`from=`** when the pointer matches a slot.

### Version bump

- Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are **`2.8.4`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.8.3] - 2026-05-14

### Fixed

- **Web log tee thread safety** ([`lib/webLogBuffer/webLogBuffer.cpp`](lib/webLogBuffer/webLogBuffer.cpp)): `Log` output is teed to Serial and the portal ring from **multiple FreeRTOS tasks** (e.g. **AsyncTCP** bridge receive vs `loop`). The ring mutex previously covered only commits, not per-character **`lineBuf` / `lineLen`** assembly or **`Serial::write`**, which could corrupt the tee and crash in **`Print::write`** (see [issue #4](https://github.com/shomanjk/esp32_balboa_spa/issues/4) discussion / serial stacks implicating **`cacheRead::processFragment`**). The mutex is now **recursive** and held for the full **`WebLogTee::write`** path (including **`write(const uint8_t*, size_t)`** bulk path).
- **Bridge fragment log lifetime** ([`lib/spaMessage/cacheRead.cpp`](lib/spaMessage/cacheRead.cpp)): `processFragment` keeps the **`msgToString`** result in a **local `String`** before passing **`c_str()`** into logging.

### Version bump

- Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are **`2.8.3`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.8.2] - 2026-05-13

### Added

- **Optional fault capture (`DIAG_FAULT_CAPTURE`)** ([`lib/faultCapture/`](lib/faultCapture/), [`src/main.ino`](src/main.ino), [`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): RTC slow-memory ring of short `[fault] …` lines (survives panic reboot), boot line on panic/WDT reset, bridge TCP lifecycle / send-fail / write-queue-full hooks, and **`faultLog`** on **`GET /api/version`**. Enabled only for **`M5AtomLite-tub`** / **`M5AtomLite-tub-ota`** in [`platformio.ini`](platformio.ini). With the flag, high-rate **`[BridgeDiag]`** lines use **VERBOSE** so default **WARNING** log level stays usable; key bridge events log at **WARNING**.

### Version bump

- Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are **`2.8.2`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.8.1] - 2026-05-13

### Fixed

- **Bridge / BWA reconnect stability** ([`lib/spaMessage/spaMessage.cpp`](lib/spaMessage/spaMessage.cpp)): When the spa RS485 **`spaWriteQueue`** is full, failed enqueues now **`delete`** the pending `SpaWriteQueueMessage` instead of leaking heap on every dropped frame. A fast bridge client (e.g. Balboa Worldwide app) could previously exhaust memory and trigger an **exception/panic** shortly after connect ([issue #4](https://github.com/shomanjk/esp32_balboa_spa/issues/4)).

### Version bump

- Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are **`2.8.1`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.8.0] - 2026-05-11

### Changed

- **Portal root URL** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): **`/`** now serves **Spa Status** (same as **`/status`**); **`/state`** remains the ESP State page.
- **Portal `/status` temperature range** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Low/high setpoint band cards are the controls for switching range (Balboa button **80**); the separate Low/High pill row was removed. Inactive bands show hover/focus affordances; hint text mentions clicking a range.

### Version bump

- Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are **`2.8.0`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.7.4] - 2026-05-05

### Added

- **Portal `/state` GitHub Sponsors button** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp), [`src/main.h`](src/main.h)): Small official embed next to **Check for updates**; iframe `src` defaults to `https://github.com/sponsors/<FIRMWARE_REPO_OWNER>/button`, overridable via `FIRMWARE_SPONSOR_BUTTON_SRC` (see [`src/config-example.h`](src/config-example.h)).

### Changed

- **Portal `/status` Spa and heating** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): **Heater State** (idle vs actively heating) uses a second hero card beside **Spa State** on wide viewports; on narrow screens the heater card stacks first. **`needsHeat`** remains only under **Raw status codes** (not shown on the hero). **Flame** icon for the heater tile; **Heating (active)** / **alternate stage** use a slow glow pulse (disabled when **`prefers-reduced-motion`**). Flame stroke is **red** (`#d32f2f`) in all heater states.
- **Portal `/state` Firmware Update card** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Two-column **This gateway** / **GitHub latest** layout with aligned pills (`v…` display), **Check for updates** and sponsor iframe on one non-wrapping actions row (horizontal scroll on very narrow screens), and shorter status text without repeating versions.

### Version bump

- Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are **`2.7.4`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.7.3] - 2026-05-04

### Fixed

- **Portal `/status` history charts** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Chart icons next to **Current temp** and **Spa and heating** now trigger the same lazy-load as **Load history charts**, then scroll to the matching section. Charts render again after load (chart data is returned as JSON and drawn from the page script; `<script>` injected via `innerHTML` is not executed by browsers).

### Version bump

- Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are **`2.7.3`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.7.2] - 2026-05-04

### Changed

- **Portal mobile navigation** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): On viewports **640px and below**, the five portal links use a **`details`/menu** row (current page title + Menu) with a sticky summary bar; after scroll, a compact bar hides the “Menu” label. **Desktop** keeps the existing five-link `.top-nav`. Inline controls (equip, range, temp units, log controls) are excluded from the global full-width `button` rule on small screens.

### Version bump

- Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are **`2.7.2`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.7.1] - 2026-05-03

### Fixed

- **Portal HTML encoding** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): `sendHtmlWithEtag` responses use `Content-Type: text/html; charset=utf-8` so UTF-8 degree symbols and dashes render correctly on mobile clients.
- **`/status` truncated or unstyled page** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Assemble `/status` with explicit `String` appends after materializing `headStatus` (avoids a long temporary chain correlated with rare responses missing `<head>` / CSS). If the body does not start with `<html>`, a **serial error** is logged before send.
- **`/status` verbose length log** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Logged payload length is captured **before** `sendHtmlWithEtag` moves the `String` (the value after send was always **0**).

### Changed

- **`/status` equipment grid** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Equipment cards use tinted backgrounds and a left accent bar for **off** (neutral), **low** (amber, multi-speed pumps), and **on**/**high** (green); live polling keeps classes in sync with `/api/status/summary`.
- **`/status` large HTML send path** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): `sendHtmlWithEtag` uses ESPAsyncWebServer’s **callback / fixed-length** response so the stack does not duplicate the entire HTML string inside `AsyncBasicResponse` (which could exhaust RAM and **RST** the connection). `/status` `String` reserve raised to **64000** to match the grown page. Verbose logging after send uses **length only** (never the full HTML body) to avoid `printf`-style stack pressure.

### Version bump

- Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are **`2.7.1`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.7.0] - 2026-04-30

### Added

- **Panel clock format write support (`TimeFormat`)** ([`lib/spaMessage/spaCommandDispatcher.cpp`](lib/spaMessage/spaCommandDispatcher.cpp), [`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Added SCI `device_request target_name='TimeFormat'` handling (`12`/`24`) and dispatcher support that applies the panel 12h/24h mode via Balboa `0x21` while preserving the current panel time payload.

### Changed

- **`/status` panel clock format control UX** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Added an inline 12/24 segmented toggle beside **Panel clock format** with the same confirmation dialog pattern used for temp/range writes and live polling sync for active selection.
- **`/status` temperature range control UX** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Added spa-style directional indicators inside low/high setpoint cards (`▼` low, `▲` high) and replaced dual range action buttons with a single segmented low/high toggle beneath the setpoint cards.
- **Version bump:** Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are now **`2.7.0`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.6.0] - 2026-04-30

### Added

- **Temp units command support (`TempUnits`, Balboa `0x27`)** ([`lib/spaMessage/spaCommandDispatcher.cpp`](lib/spaMessage/spaCommandDispatcher.cpp), [`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp), [`lib/mqttModule/mqttModule.cpp`](lib/mqttModule/mqttModule.cpp)): Added shared dispatcher support for set temperature scale writes, wired SCI `device_request target_name='TempUnits'` payloads (`C`/`F`), and added MQTT `Spa/<gateway>/cmd/tempUnits` handling (`C`/`Celsius`/`F`/`Fahrenheit`) with standard command-result telemetry.

### Changed

- **`/status` temp units control UX** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Replaced the larger inline temp-unit control with a compact C/F segmented toggle in the **Current Temp** row, preserved the confirmation dialog before writes, and kept live polling state sync for active unit highlighting.
- **Version bump:** Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are now **`2.6.0`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.5.1] - 2026-04-30

### Fixed

- **ESP State empty Spa Status panel removal** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Removed a leftover HTML section opener that rendered an empty **Spa Status** block on `/state`, and corrected section transitions so Wi-Fi and MQTT panels render cleanly.

### Changed

- **ESP State firmware comparison badges** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Added **Current** and **Latest** version badges near Firmware Version and wired latest badge updates from the GitHub release check.
- **Version bump:** Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are now **`2.5.1`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.5.0] - 2026-04-30

### Added

- **ESP State MQTT visibility** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp), [`lib/mqttModule/mqttModule.h`](lib/mqttModule/mqttModule.h)): Added an `/state` MQTT panel and `GET /api/mqtt` snapshot endpoint with broker/topic/discovery status and reconnect-attempt age telemetry; portal explicitly hides credentials and points operators to `src/config.h` for `MQTT_SERVER`, `MQTT_PORT`, `BROKER_LOGIN`, and `BROKER_PASS`.

### Changed

- **ESP State firmware update UX** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Moved **Check for updates** inline with **Firmware Version** and color-coded result states (checking, up-to-date, update available, error) for faster operator interpretation.
- **ESP State freshness labeling** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Moved status `lastUpdate` into the **Spa Data Freshness** table as **Status Snapshot** and clarified how it differs from configuration/info dataset freshness rows.
- **Version bump:** Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are now **`2.5.0`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.4.0] - 2026-04-30

### Changed

- **MQTT availability resilience** ([`lib/mqttModule/mqttModule.cpp`](lib/mqttModule/mqttModule.cpp)): Increased PubSubClient keepalive from 10s to 60s and socket timeout from 1s to 5s so brief broker latency or ESP32 loop stalls are less likely to trigger Home Assistant availability flapping through the retained `node/state` Last Will topic.
- **ESP32 Wi-Fi stability** ([`lib/wifiModule/wifiModule.cpp`](lib/wifiModule/wifiModule.cpp)): Disabled station Wi-Fi sleep for the always-powered gateway to reduce MQTT reconnect churn on marginal links.
- **Version bump:** Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are now **`2.4.0`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.3.0] - 2026-04-30

### Fixed

- **Web/MQTT command readiness gate for status-driven commands** ([`lib/spaMessage/spaCommandDispatcher.cpp`](lib/spaMessage/spaCommandDispatcher.cpp)): Command dispatch no longer requires both `spaStatusData` and `spaConfigurationData` to be fresh for toggle/range/time/setpoint paths; these now gate on fresh status only, fixing false `Button` rejections such as `80:off` (`error='spa status/config not ready'`) when status was present but configuration was not yet populated.
- **Home Assistant spa light entity platform** ([`lib/mqttModule/haMqttDiscovery.cpp`](lib/mqttModule/haMqttDiscovery.cpp)): Writable `light1`/`light2` discovery now publishes as HA `light` entities (instead of `switch`) for correct domain semantics; discovery retraction now also clears stale retained `switch`/`binary_sensor` light configs from prior firmware behavior.
- **Home Assistant light discovery churn/auto-disable risk** ([`lib/mqttModule/haMqttDiscovery.cpp`](lib/mqttModule/haMqttDiscovery.cpp)): Expanded equipment discovery no longer republishes spa lights as legacy `binary_sensor`/`switch` alongside writable `light` entities, and writable light discovery now publishes only when those lights are installed per spa configuration, reducing entity registry churn that can appear as entities auto-disabling/removing.

### Changed

- **Version bump:** Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are now **`2.3.0`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.2.3] - 2026-04-30

### Added

- **Firmware portal lazy-load endpoints for weak Wi-Fi** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Added `GET /api/status/histories` and `GET /api/state/littlefs` so heavy history charts and LittleFS inventory can be loaded on demand instead of inflating initial `/status`, `/state`, and `/config` page payloads.

### Changed

- **Firmware portal polling resilience on flaky links** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): `/status` now polls compact `GET /api/status/summary` with request timeouts, exponential backoff + jitter, and stale-data messaging; `/logs` now defaults websocket-first with reconnect/backoff and adaptive polling fallback.
- **Conditional page revalidation (ETag/304)** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): `/status`, `/state`, `/config`, and `/logs` now emit weak `ETag` headers and honor `If-None-Match` with `304 Not Modified` to reduce reload bandwidth over unreliable Wi-Fi.
- **Version bump:** Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are now **`2.2.3`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.2.2] - 2026-04-30

### Fixed

- **Home Assistant climate discovery temperature unit validation** ([`lib/mqttModule/haMqttDiscovery.cpp`](lib/mqttModule/haMqttDiscovery.cpp)): MQTT discovery for `climate` now publishes `temperature_unit` as Home Assistant-required `F` or `C` (instead of degree-symbol variants), preventing HA discovery rejection and associated entity disable/unavailable churn.

### Changed

- **Version bump:** Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are now **`2.2.2`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.2.1] - 2026-04-30

### Fixed

- **Home Assistant pump status for single-speed installs** ([`lib/mqttModule/haMqttDiscovery.cpp`](lib/mqttModule/haMqttDiscovery.cpp)): Single-speed pumps now publish as HA `switch` entities with normalized state mapping so reported pump states (`Low`/`High`/`On`) resolve to switch **ON** and `Off` resolves to **OFF**, fixing cases where the spa/web status showed pump on but HA immediately flipped the switch back off.

### Changed

- **Pump discovery by capability remains automatic** ([`lib/mqttModule/haMqttDiscovery.cpp`](lib/mqttModule/haMqttDiscovery.cpp)): Single-speed pumps use `switch` controls while two-speed pumps continue to use `select` (`Off`/`Low`/`High`), keeping compatibility across spa configurations.
- **Version bump:** Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are now **`2.2.1`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.2.0] - 2026-04-30

### Added

- **MQTT command dispatch (`cmd/#`)** ([`lib/mqttModule/mqttModule.cpp`](lib/mqttModule/mqttModule.cpp), [`lib/spaMessage/spaCommandDispatcher.cpp`](lib/spaMessage/spaCommandDispatcher.cpp)): MQTT now subscribes `Spa/<gateway>/cmd/#` and dispatches `setTemp`, `setTime`, `syncTime`, `mode`, `preset`, and `button/<code>` to the shared command dispatcher (`0x11` / `0x20` / `0x21`) instead of echoing payloads.
- **MQTT command result telemetry** ([`lib/mqttModule/mqttModule.cpp`](lib/mqttModule/mqttModule.cpp)): Each command publish emits JSON result on `Spa/<gateway>/cmd/result` with `target`, `value`, `accepted`, and `reason` for HA automations and troubleshooting.
- **Writable HA discovery controls** ([`lib/mqttModule/haMqttDiscovery.cpp`](lib/mqttModule/haMqttDiscovery.cpp)): Discovery now includes writable `climate` (`spa_controls`), load switches, per-pump controls, panel-time sync button, and diagnostic last-command-result sensor.

### Changed

- **Pump HA platform by capability** ([`lib/mqttModule/haMqttDiscovery.cpp`](lib/mqttModule/haMqttDiscovery.cpp)): Installed single-speed pumps are discovered as `switch` entities (`Off`/`Low`), while two-speed pumps are discovered as `select` entities (`Off`/`Low`/`High`) with stable slot naming (`Spa pump N`).
- **Shared button-state toggle logic** ([`lib/spaMessage/spaCommandDispatcher.cpp`](lib/spaMessage/spaCommandDispatcher.cpp), [`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Web SCI and MQTT command paths now share centralized toggle-count helpers for pump/load/range/mode state convergence.
- **Version bump:** Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are now **`2.2.0`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.1.0] - 2026-04-30

### Added

- **Panel clock set (`0x21`)** ([`lib/spaMessage/spaCommandDispatcher.cpp`](lib/spaMessage/spaCommandDispatcher.cpp), [`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): SCI **`device_request`** target **`SystemTime`** with `HH:MM` dispatches **`spaSetSpaPanelClockTime`** (Balboa Set Time per [protocol.md](https://github.com/ccutrer/balboa_worldwide_app/blob/main/doc/protocol.md)); **`/status`** adds time picker, **Send to spa**, and **Sync from gateway** (`gatewayTimeHHMM` from **`GET /api/status/controls`**).

### Changed

- **`/status` panel clock section** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): **Panel clock and filter cycles** block moved **below Equipment**; human-readable **Panel clock format** (12h vs 24h) with raw flag in `title`; **`GET /api/status/controls`** JSON adds **`panelTime`**, **`clockFormat`**, **`clockModeRaw`**, **`filterModeText`**, **`gatewayTimeHHMM`** for live polling (`DynamicJsonDocument` **2560**).
- **Version bump:** Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are now **`2.1.0`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [2.0.0] - 2026-04-29

### Highlights

- **Reliable spa command writes (major milestone):** Tub-side **Balboa `0x11` (toggle)** and **`0x20` (set temperature)** frames built in [`lib/spaMessage/spaCommandDispatcher.cpp`](lib/spaMessage/spaCommandDispatcher.cpp) now use the same **`addCRC`** path as [`lib/localRS485Communication/rs485.cpp`](lib/localRS485Communication/rs485.cpp) (Balboa CRC-8 per [protocol.md](https://github.com/ccutrer/balboa_worldwide_app/blob/main/doc/protocol.md)), fixing wire-invalid checksums that prevented the controller from acting on portal-issued commands. The web SCI layer (`device_request` **`Button`** / **`SetTemp`**) dispatches through this stack; **`spaSetTargetTemperature`** enforces protocol setpoint bands by **°F/°C** and **high/low range**. Together with **`/status`** controls, polling, and range UX below, **2.0.0** marks the fork line where operators can trust gateway-initiated writes in normal use. *(The CRC alignment first shipped as **1.8.1**; it is included in the story for this release.)*

### Added

- **`/status` temperature range UX** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): High/low setpoint bands with clear active highlighting, **Use high range** / **Use low range** buttons (Balboa toggle item **80** / `0x50`), scope label on Set temp, and polling updates for bands, range buttons, and setpoint input bounds.
- **`/status` Spa and heating panel** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): At-a-glance hero (spa state + thermometer icon), colored chips for heating activity and needs-heat, short mode vs state hint, chart jump link to heater history, collapsible raw codes, and **polling** updates without reload.
- **`spaProtocolActiveSetpointBand`** ([`lib/spaMessage/spaCommandDispatcher.h`](lib/spaMessage/spaCommandDispatcher.h), [`lib/spaMessage/spaCommandDispatcher.cpp`](lib/spaMessage/spaCommandDispatcher.cpp)): Central Balboa **`0x20`** limits by °F/°C and active high/low range ([protocol.md](https://github.com/ccutrer/balboa_worldwide_app/blob/main/doc/protocol.md)); **`spaSetTargetTemperature`** rejects out-of-band values before enqueueing.

### Changed

- **`/status` page header** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Removed the **Data sync** panel; **snapshot freshness** (relative age since last bus status apply + gateway-local timestamp) appears as muted text to the right of **Spa Status** (stacks on narrow view) and refreshes with the existing **`/api/status/controls`** poll.
- **`/state` advanced** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): **`spaStatusData.magicNumber`** is labeled **Spa status struct magic (ESP RAM)** with a short note that it is firmware-side (expected **`0x12345678`** after init), not from the spa controller.
- **`/status` ESP memory panel removed** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Free heap / PSRAM / stack now appear only on **`/state`** (System Health; PSRAM and stack under **Show advanced diagnostics**), avoiding duplicate gateway metrics on the spa-focused page.
- **`GET /api/status/controls`** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): JSON now includes **`tempRange`**, **`highSetTemp`**, **`lowSetTemp`**, **`setTempMin`**, **`setTempMax`**, spa/heating fields (**`spaState`**, **`spaStateText`**, **`initMode`**, **`initModeText`**, **`heatingMode`**, **`heatingModeText`**, **`heatingState`**, **`heatingStateText`**, **`needsHeat`**), and snapshot helpers (**`snapshotAgeSec`**, **`snapshotAtLocal`**, **`snapshotMeta`**) for the `/status` header line (`DynamicJsonDocument` size **2048**).
- **Version bump:** Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are now **`2.0.0`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [1.8.1] - 2026-04-29

### Fixed

- **Spa command CRC matches RS485 / protocol** ([`lib/spaMessage/spaCommandDispatcher.cpp`](lib/spaMessage/spaCommandDispatcher.cpp), [`lib/localRS485Communication/rs485.h`](lib/localRS485Communication/rs485.h)): Outbound `0x11` / `0x20` frames from the shared dispatcher now use the same **`addCRC`** path as [`lib/localRS485Communication/rs485.cpp`](lib/localRS485Communication/rs485.cpp) (Balboa CRC-8 per [protocol.md](https://github.com/ccutrer/balboa_worldwide_app/blob/main/doc/protocol.md)) instead of a divergent CRC implementation that produced wire-invalid checksums.

### Changed

- **Version bump:** Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are now **`1.8.1`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [1.8.0] - 2026-04-29

### Added

- **`/status` equipment live refresh (polling)** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): The status page now polls **`GET /api/status/controls`** every 2 seconds (skipped while the browser tab is hidden) and updates equipment values and **Turn On / Turn Off** button targets in place, so passive spa changes no longer require a manual full-page reload.

### Changed

- **`GET /api/status/controls` payload** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Response now includes **`circ`** (circulation pump on/off) and **`pump1Config`–`pump6Config`** so the polled UI can match single-speed vs multi-speed pump display semantics used on `/status`.
- **`/status` equipment markup** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Equipment cards expose stable **`data-equip`** and **`data-role="value"`** hooks for the polling script.
- **Portal shared CSS / `<head>`** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): Top navigation link styles are consolidated into the shared `style` macro and the duplicate inline nav block was removed from `head`, avoiding redundant CSS and keeping nav appearance consistent across pages.
- **Status `lastUpdate` display** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp)): `lastUpdate` rows now show human-readable local time only (the collapsible raw Unix epoch detail was removed).
- **`/state` noise-reduction layout** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp), [`README.md`](README.md)): Reorganized ESP State into an operator-first view (system health + Wi-Fi + compact spa freshness), moved low-value diagnostics behind a `Show advanced diagnostics` toggle, and added advanced `API Shortcuts` links (`/api/wifi`, `/api/version`, `/api/rs485`, `/api/rs485/raw`, `/api/rs485/history`) plus a header jump link.
- **Version bump:** Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are now **`1.8.0`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).

## [1.7.2] - 2026-04-29

### Added

- **Web log viewer** ([`lib/webLogBuffer/`](lib/webLogBuffer/), [`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp), [`src/main.ino`](src/main.ino)): Portal **`/logs`** tab with include/exclude filters, pause, HTTP poll of **`GET /api/logs`**, optional **`WebSocket /api/logs/ws`** tail (broadcast from main loop, not from the logger path), and **`GET`/`POST /api/logs/config`** for runtime **`Log.setLevel`** (clamped to compile-time `LOG_LEVEL`). Serial output unchanged; ring buffer holds the last 80 lines (~15 KB RAM).
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
- **Decoded status-byte diagnostics in Light1 CTS endpoint** ([`lib/spaWebServer/spaWebServer.cpp`](lib/spaWebServer/spaWebServer.cpp), [`docs/diag-light1-next-cts-live-run-decoded.json`](docs/diag-light1-next-cts-live-run-decoded.json), [`docs/command-write-debug-log.md`](docs/command-write-debug-log.md)): Added parsed status-byte snapshots (`hf`, `pp`, `lf`, `stRaw`) to before/after diagnostic payloads and captured decoded post-OTA trial artifacts; no Light1 state transitions were observed.
- **Rolling-window CTS-edge Light1 live artifact** ([`docs/diag-light1-next-cts-window-live-run.json`](docs/diag-light1-next-cts-window-live-run.json), [`docs/command-write-debug-log.md`](docs/command-write-debug-log.md)): Captured multi-sample status windows around next-CTS Light1 toggles to detect transient flips; command fire counters advanced as expected but no Light1 or `lf`-byte transitions were observed in sampled windows.
- **Rolling-window CTS-edge A/B artifact** ([`docs/diag-light1-next-cts-window-ab-live-run.json`](docs/diag-light1-next-cts-window-ab-live-run.json), [`docs/command-write-debug-log.md`](docs/command-write-debug-log.md)): Added live A/B trials for `dest`, `pad`, and pre-housekeeping variants around next-CTS Light1 toggles; successful sampled runs still showed no Light1 or `lf`-byte transitions.

### Changed

- **Version bump:** Firmware **`VERSION`** and **`ANALYTICS_VERSION`** are now **`1.7.2`** ([`src/main.h`](src/main.h), [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h)).
- **Wi-Fi offline self-heal watchdog** ([`lib/wifiModule/wifiModule.cpp`](lib/wifiModule/wifiModule.cpp), [`lib/wifiModule/wifiModule.h`](lib/wifiModule/wifiModule.h), [`src/config-example.h`](src/config-example.h)): Added timed offline restart recovery (default 10 minutes offline, minimum uptime 2 minutes, throttled progress logs). When connectivity returns, the watchdog state clears and logs recovery duration.
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
- **M5 Atom Lite (tub-side):** PlatformIO environment `M5AtomLite-tub` using `board = m5stack-atom`, tub-side build flags (`LOCAL_CLIENT`, `LOCAL_CONNECT`, `BRIDGE`), and `upload_speed = 1500000` ([`platformio.ini`](platformio.ini)).
- **`src/config-example.h`:** Documented optional UART/GPIO settings for **M5 Atom Lite + M5 Unit RS485** (Grove wiring, `TX485_Rx` / `TX485_Tx`, `AUTO_TX` guidance) with links to M5 documentation.
- **`README.md`:** Section **“M5 Atom Lite + M5 RS485 (tub-side)”** describing the new env, wiring to [Unit RS485](https://docs.m5stack.com/en/unit/rs485), `config.h` setup, and power notes.

### Changed

- **`src/config-example.h`:** Clarified RS485 pin comments for generic ESP32 vs M5; default GPIO16/17 retained for existing setups.

[Unreleased]: https://github.com/shomanjk/esp32_balboa_spa/compare/v2.26.2...HEAD
[2.26.2]: https://github.com/shomanjk/esp32_balboa_spa/compare/v2.26.1...v2.26.2
[2.26.1]: https://github.com/shomanjk/esp32_balboa_spa/compare/v2.26.0...v2.26.1
[2.26.0]: https://github.com/shomanjk/esp32_balboa_spa/compare/v2.25.0...v2.26.0
[2.25.0]: https://github.com/shomanjk/esp32_balboa_spa/compare/v2.24.0...v2.25.0
[2.24.0]: https://github.com/shomanjk/esp32_balboa_spa/compare/v2.23.0...v2.24.0
[2.23.0]: https://github.com/shomanjk/esp32_balboa_spa/compare/v2.22.0...v2.23.0
[2.22.0]: https://github.com/shomanjk/esp32_balboa_spa/compare/v2.21.1...v2.22.0
[2.21.1]: https://github.com/shomanjk/esp32_balboa_spa/compare/v2.21.0...v2.21.1
[2.21.0]: https://github.com/shomanjk/esp32_balboa_spa/compare/v2.20.1...v2.21.0
[2.20.1]: https://github.com/shomanjk/esp32_balboa_spa/compare/v2.20.0...v2.20.1
[2.20.0]: https://github.com/shomanjk/esp32_balboa_spa/compare/v2.19.1...v2.20.0
[2.19.1]: https://github.com/shomanjk/esp32_balboa_spa/compare/v2.19.0...v2.19.1
[2.19.0]: https://github.com/shomanjk/esp32_balboa_spa/compare/v2.18.6...v2.19.0
[2.18.6]: https://github.com/shomanjk/esp32_balboa_spa/compare/v2.18.5...v2.18.6
[2.18.5]: https://github.com/shomanjk/esp32_balboa_spa/compare/v2.18.4...v2.18.5
[2.18.4]: https://github.com/shomanjk/esp32_balboa_spa/compare/v2.18.3...v2.18.4
[2.18.3]: https://github.com/shomanjk/esp32_balboa_spa/compare/v2.18.2...v2.18.3
[2.18.2]: https://github.com/shomanjk/esp32_balboa_spa/compare/v2.18.1...v2.18.2
[2.18.1]: https://github.com/shomanjk/esp32_balboa_spa/compare/v2.18.0...v2.18.1
[2.18.0]: https://github.com/shomanjk/esp32_balboa_spa/compare/v2.17.1...v2.18.0
[2.17.1]: https://github.com/shomanjk/esp32_balboa_spa/compare/v2.17.0...v2.17.1
[2.17.0]: https://github.com/shomanjk/esp32_balboa_spa/compare/v2.16.0...v2.17.0
[2.8.6]: https://github.com/shomanjk/esp32_balboa_spa/compare/v2.8.5...v2.8.6
[2.4.0]: https://github.com/shomanjk/esp32_balboa_spa/compare/v2.3.0...v2.4.0
[2.3.0]: https://github.com/shomanjk/esp32_balboa_spa/compare/v2.2.3...v2.3.0
[2.2.3]: https://github.com/shomanjk/esp32_balboa_spa/compare/v2.2.2...v2.2.3
[2.2.2]: https://github.com/shomanjk/esp32_balboa_spa/compare/v2.2.1...v2.2.2
[2.2.1]: https://github.com/shomanjk/esp32_balboa_spa/compare/v2.2.0...v2.2.1
[2.2.0]: https://github.com/shomanjk/esp32_balboa_spa/compare/v2.1.0...v2.2.0
[2.1.0]: https://github.com/shomanjk/esp32_balboa_spa/compare/v2.0.0...v2.1.0
[2.0.0]: https://github.com/shomanjk/esp32_balboa_spa/compare/v1.8.1...v2.0.0
[1.8.1]: https://github.com/shomanjk/esp32_balboa_spa/compare/v1.8.0...v1.8.1
[1.8.0]: https://github.com/shomanjk/esp32_balboa_spa/compare/v1.7.2...v1.8.0
[1.7.2]: https://github.com/shomanjk/esp32_balboa_spa/compare/v1.6.1...v1.7.2
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
