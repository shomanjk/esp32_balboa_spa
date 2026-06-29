# Command Write Debug Log

This log tracks attempted command-write solutions and measured outcomes so we do not duplicate experiments.

Live gateway diagnostic exports (`docs/diag-*.json`, `docs/telnet-*.txt`) are **gitignored** — same policy as generated bridge run outputs. Shape reference: [`diag-light1-next-cts-live-run.example.json`](diag-light1-next-cts-live-run.example.json).

## 2026-06-22 - MQTT filter schedule writes + running telemetry

- **Implementation:** [`mqttModule.cpp`](../lib/mqttModule/mqttModule.cpp) dispatches **`Spa/<gateway>/cmd/filter`** (JSON, merge from cache) and granular **`cmd/filter/filter{1,2}/{start,duration,enabled}`** via shared helpers in [`spaCommandDispatcher.cpp`](../lib/spaMessage/spaCommandDispatcher.cpp) → **`spaSetFilterCycles(..., SPA_COMMAND_SOURCE_MQTT)`**. Read telemetry: **`status/filter1_running`**, **`status/filter2_running`** from live **`filterMode`** ([`spaMqttMessage.cpp`](../lib/spaMessage/spaMqttMessage.cpp)); HA **`binary_sensor`** discovery in [`haMqttDiscovery.cpp`](../lib/mqttModule/haMqttDiscovery.cpp).
- **Firmware build:** `pio run -e M5AtomLite-tub` succeeds (compile-only verification).
- **Outcome:** *Pending tub-side / MQTT confirmation* — run matrix below.

### MQTT validation matrix (pending)

Pre-flight: subscribe to **`Spa/<gateway>/filterSettings/#`** and **`Spa/<gateway>/status/filter1_running`**; confirm filter cache ready (`filterSettings/lastUpdate` non-zero).

| Step | Action | Pass criteria | Result |
|------|--------|---------------|--------|
| M1 | **`cmd/filter/filter1/start`** → `09:00` | `cmd/result` accepted; `filterSettings/filt1Hour`/`filt1Minute` update | *pending* |
| M2 | **`cmd/filter/filter1/duration`** → `04:00` | Duration fields update; Filter 1 unchanged start if M1 ran first | *pending* |
| M3 | **`cmd/filter`** partial JSON (only `filter2`) | Omitted Filter 1 fields unchanged on tub | *pending* |
| M4 | **`cmd/filter/filter2/enabled`** → `false` | Filter 2 disabled; byte +4 = `0x00` in raw hex | *pending* |
| M5 | During active cycle 1 | **`status/filter1_running`** = `On`; cycle 2 off unless mode 3 | *pending* |

## 2026-06-10 - Filter cycle writes (`0x23`) + config export/import

- **Implementation:** [`spaSetFilterCycles`](../lib/spaMessage/spaCommandDispatcher.cpp) queues **`0a bf 23`** + 8 payload bytes (Filter 2 disabled → byte +4 = **`0x00`**; enabled → **`(hour & 0x7F) | 0x80`**), then calls [`spaRequestFilterSettings()`](../lib/spaMessage/spaMessage.cpp). SCI routing in [`spaWebServer.cpp`](../lib/spaWebServer/spaWebServer.cpp): read **`Request`/`Filters`**, write **`Filters`** + base64 → **`decodeSciFilterBlob`** bytes `[4..11]` (matches [balboa-spa `balboa.js`](https://github.com/jozefnad/balboa-spa/blob/master/src/assets/balboa.js) `parseFilterCycles` / `generateFilterCyclesArray`). JSON: **`GET/POST /api/config/filter`**, **`GET /api/config/export`**, **`POST /api/config/import`** ([`spaConfigExport.cpp`](../lib/spaWebServer/spaConfigExport.cpp)).
- **Firmware build:** `pio run -e M5AtomLite-tub` succeeds (compile-only verification).
- **balboa-spa SCI parity (code review):** `getFilterCycles()` → `target_name="Request">Filters`; `setFilterCycles()` → `target_name="Filters">${base64}`; decoded indices `[4..11]` align with firmware parser. No submodule changes required.
- **Outcome:** *Pending tub-side confirmation* — run matrix below on live spa and record pass/fail per step.

### Tub validation matrix (pending)

Pre-flight: note model from **`/config`** → Controller identity; capture baseline filter section + raw **`0x23`** hex; confirm panel clock on **`/status`**.

| Step | Action | Pass criteria | Result |
|------|--------|---------------|--------|
| A | **`POST /api/config/filter`**: shift Filter 1 start +15 min | GET + raw hex + panel menu match | *pending* |
| B | **`POST /api/config/filter`**: change Filter 1 duration | Same | *pending* |
| C | Enable Filter 2 with schedule | `filt2Enable=yes`, bit 7 set in hex | *pending* |
| D | Disable Filter 2 | byte +4 = `0x00` in hex | *pending* |
| E | balboa-spa tab: same edit as A | SCI accepted, readback match | *pending* |
| F | Reboot gateway | Schedule persists on tub | *pending* |

Backup/restore (optional after A–F):

| Step | Action | Pass criteria | Result |
|------|--------|---------------|--------|
| G | **`GET /api/config/export`** with fresh data | JSON has `writable` + `snapshot`; file downloads | *pending* |
| H | Re-import same file (`dryRun: true`) | No warnings; writable sections listed | *pending* |
| I | Apply same file | Readback matches | *pending* |
| J | Edit filter in file, re-import | Only filter changes on tub | *pending* |
| K | Import from different tub export | Warning + blocked until `force` | *pending* |

## 2026-05-14 - Bridge / BWA `LoadProhibited` in `Print::write` (issue #4 follow-up)

- **Symptom:** Guru **`LoadProhibited`**, **`EXCVADDR: 0x00000008`**, decoded stack through **`AsyncTCP` → `bridge::clientDataAvailable` → `cacheRead` → `processFragment`** (fragment **`[BridgeDiag]`** log line) into **ArduinoLog → `Print::write`**.
- **Hypothesis (confirmed in code review):** **`WebLogTee`** assembled **`lineBuf` / `lineLen`** and called **`Serial::write`** without the same mutex used for ring commits, while **`Log`** can run from **AsyncTCP** and **`loop`** concurrently → torn tee state / undefined behavior.
- **Fix:** [`lib/webLogBuffer/webLogBuffer.cpp`](../lib/webLogBuffer/webLogBuffer.cpp) — **recursive mutex** held for the full **`write`** path (including bulk **`write(const uint8_t*, size_t)`**); plus **`cacheRead`** local **`String`** for **`msgToString`** before **`c_str()`** in **`processFragment`**.

## 2026-04-30 - MQTT command path parity with web dispatcher

- **Implementation:** MQTT callback in [`mqttModule.cpp`](../lib/mqttModule/mqttModule.cpp) now dispatches `Spa/<gateway>/cmd/#` topics to shared helpers in [`spaCommandDispatcher.cpp`](../lib/spaMessage/spaCommandDispatcher.cpp) (`setTemp`, `setTime`, `syncTime`, `mode`, `preset`, `button/<code>`), and publishes JSON outcomes on `Spa/<gateway>/cmd/result`.
- **Parity hardening:** Web SCI `Button` path now calls shared `spaToggleCountForButtonRequest` logic, so web and MQTT compute the same multi-toggle sequences (including pump/range/mode cases).
- **Outcome:** Pending hardware confirmation for each MQTT topic family; use the smoke checklist from the HA MQTT control plan and record accepted/rejected `cmd/result` payloads here after tub-side verification.

## 2026-04-29 - Panel clock write (`0x21` / `SystemTime`)

- **Implementation:** [`spaSetSpaPanelClockTime`](../lib/spaMessage/spaCommandDispatcher.cpp) queues **`0a bf 21 HH MM`** with CRC; hour byte bit 7 set when status **`clockMode`** has **`0x02`** (24-hour panel format). Web SCI **`SystemTime`** and **`/status`** controls dispatch the same path.
- **Outcome:** *Pending tub-side confirmation* — add a line here after you verify the physical panel clock advances to the requested `HH:MM`.

## 2026-04-29 - Release **2.0.0** (major): trusted tub-side writes

- **Milestone:** Tagged line **`2.0.0`** — firmware **`VERSION`** / **`ANALYTICS_VERSION`** ([`src/main.h`](../src/main.h), [`lib/Analytics/Analytics.h`](../lib/Analytics/Analytics.h)). [CHANGELOG.md](../CHANGELOG.md) summarizes portal command reliability, **`/status`** UX (range, heating panel, polling), and protocol setpoint validation.
- **Operator expectation:** Balboa **`0x11`** / **`0x20`** frames from [`spaCommandDispatcher`](../lib/spaMessage/spaCommandDispatcher.cpp) use shared **`addCRC`** with the RS485 path; combined with SCI dispatch in [`spaWebServer.cpp`](../lib/spaWebServer/spaWebServer.cpp), **commands sent from the gateway web UI are expected to apply on real hardware** when the spa is ready and the frame matches the pack.

## 2026-04-29 - Dispatcher CRC aligned with RS485 (`VERSION` 1.8.1)

- **Change:** [`lib/spaMessage/spaCommandDispatcher.cpp`](../lib/spaMessage/spaCommandDispatcher.cpp) now calls shared [`addCRC`](../lib/localRS485Communication/rs485.cpp) (same CRC-8 as [`protocol.md`](https://github.com/ccutrer/balboa_worldwide_app/blob/main/doc/protocol.md) / RS485 path) instead of a divergent local implementation.
- **OTA:** `M5AtomLite-tub-ota` upload to the tub-side unit succeeded when targeting the device **LAN IP** (`pio run -e M5AtomLite-tub-ota -t upload --upload-port <ip>`); hostname-only espota had been aborting mid-transfer in some environments.
- **Outcome (Light 1):** After OTA, **`GET /api/diag/light1_next_cts`** reported the framed toggle as `7e 07 0a bf 11 11 00 93 7e` (CRC `0x93`). **Confirmed at the physical spa:** Light 1 was **on** (human observation at the tub), matching the intended command effect even though the short diagnostic window had previously reported `light1Changed=false` from sampled status alone.
- **Outcome (Pump 2 on):** **`GET /api/diag/toggle?item=5`** (Balboa item `0x05` = pump 2) queued `7e 07 0a bf 11 05 00 90 7e`. **`GET /api/status/controls`** before: `pump2=0`, `pump2On=false`; after ~4s: `pump2=2`, `pump2On=true` (spa applied the toggle over RS485).

## 2026-04 - Command write bring-up (web + MQTT path)

- Added shared dispatcher for outbound commands (`toggle` and `set_temp`) to centralize frame construction and queueing.
- Wired web SCI `device_request` paths (`Button`, `SetTemp`) through dispatcher.
- Added status-page controls and post-send verification polling (`/api/status/controls`).

Outcome:
- Firmware accepted outbound requests, but spa state did not reliably change for toggle actions.

## 2026-04 - Wire-level alignment attempts

- Forced destination for command frames to `WIFI_MODULE_ID` (`0x0A`) to match known-good request framing in project.
- Updated toggle payload format to include required pad byte (`II 00`) for `0x11`.

Outcome:
- Commands still accepted by firmware but observed as no-op at spa state level.

## 2026-04 - Diagnostic instrumentation pass

- Added `GET /api/diag/toggle` to emit exact queued frame (`frame`) plus acceptance metadata for `item`, `dest`, and `pad` A/B checks.
- Ran matrix variants (destination mode and pad mode) during live testing.

Outcome:
- Acceptance path remained healthy; no conclusive state-change improvement across tested variants.

## 2026-04 - Single-speed pump normalization pass

- Normalized configured 1-speed pump display/control matching to binary on/off semantics while retaining raw values in API payloads.
- Added normalized booleans (`pumpNOn`) to `/api/status/controls` and used them in status-page convergence checks.

Outcome:
- Improved interpretation and reduced false multi-speed assumptions; did not by itself resolve wire-level no-op.

## 2026-04 - Timed retry diagnostics

- Added `GET /api/diag/toggle_sequence` to run controlled repeat/gap/observe sequences and return before/after snapshots plus per-attempt frame metadata.

Suggested usage for Pump1:

1. Single attempt:
   - `/api/diag/toggle_sequence?item=4&repeats=1&gap_ms=1200&observe_ms=5000`
2. Retry with spacing:
   - `/api/diag/toggle_sequence?item=4&repeats=2&gap_ms=2000&observe_ms=7000`
3. Longer observe window:
   - `/api/diag/toggle_sequence?item=4&repeats=1&gap_ms=1200&observe_ms=12000`

Decision hints:
- `allAccepted=true` and `pump1Changed=false`: likely controller/item/timing semantics mismatch, not local queue rejection.
- Early rejected attempt: inspect `reason` and queue/readiness conditions.

### 2026-04-28 live run results (Pump1 item `4`)

Environment:
- Firmware path: `M5AtomLite-tub-ota`
- Command frame emitted: `7e 07 0a bf 11 04 00 13 7e`
- Baseline: `pump1=0`, `pump1On=false`

Runs:
1. `repeats=1, gap_ms=1200, observe_ms=5000`
   - `allAccepted=true`, `pump1Changed=false`
2. `repeats=2, gap_ms=2000, observe_ms=7000`
   - `allAccepted=true`, `pump1Changed=false`
3. `repeats=1, gap_ms=1200, observe_ms=12000`
   - `allAccepted=true`, `pump1Changed=false`

Interpretation:
- Timing/retry variation did not produce state convergence for Pump1 in this controller setup.
- Transport/queue acceptance appears healthy; likely remaining issue is controller-specific command semantics (item mapping, destination/channel nuance, or additional prerequisites beyond current frame shape).

## 2026-04 - Bridge-first raw troubleshooting harness

- Added bridge observability breadcrumbs (`[BridgeDiag]`) across ingress, cache forwarding, queueing, CTS dequeue, and RS485 send.
- Added raw bridge harness script: `scripts/bridge_raw_tester.py`.
- Added repeatable matrix template: `docs/bridge-raw-command-matrix.example.json`.

Harness I/O contract:

- Input:
  - `--host`, optional `--port` (default `4257`)
  - single case: `--frame-hex`, optional `--label`, `--retries`, `--cooldown-ms`, `--timeout-ms`
  - matrix mode: `--matrix <json>` where each case supports `label`, `frame_hex`, optional `retries`, `cooldown_ms`, `timeout_ms`, `expect_hex`
- Output:
  - stdout JSON plus optional `--out <file>`
  - per-case attempts with `startedMs`, `sentHex`, `rxHex`, `ok`, `error`, `durationMs`, optional `expectMatched`

Matrix execution status:

- Dry-run validation completed:
  - `python3 scripts/bridge_raw_tester.py --host 127.0.0.1 --matrix docs/bridge-raw-command-matrix.example.json --dry-run --out docs/bridge-raw-last-run.json`
  - Result: parser/runner/output contract verified (`caseCount=2`, `ok=true` in dry-run mode)
- Live matrix rows for command families under test:
  - `0x11` (toggle): ready to execute via bridge using known frame `7e 07 0a bf 11 04 00 13 7e`
  - `0x20` (set temp): matrix includes `set_temp_100f` frame `7e 06 0a bf 20 64 c1 7e`

### 2026-04-28 live bridge harness run (`spa-XXXXXXXXXXXX.local`)

Command:

- `python3 scripts/bridge_raw_tester.py --host spa-XXXXXXXXXXXX.local --matrix docs/bridge-raw-command-matrix.example.json --out docs/bridge-raw-live-run.json`

Results:

- `toggle_pump1_item4` (`7e 07 0a bf 11 04 00 13 7e`)
  - `ok=true`
  - `rxHex=""` (no immediate payload returned on bridge socket)
  - `durationMs=6563`
- `set_temp_100f` (`7e 06 0a bf 20 64 c1 7e`)
  - `ok=true`
  - `rxHex="7e 1d ff af 13 00 03 64 08 39 00 28 64 00 04 08 00 00 02 00 00 00 00 02 02 4c 00 00 02 62 7e"`
  - `durationMs=6556`

Interpretation:

- Bridge connectivity and write path are functioning for both command families under this harness.
- `0x20` produced an immediate bridge response frame, so next analysis should decode/compare this response against expected post-command status behavior.

### 2026-04-28 live repeatability run (`0x20`, retries=3)

Command:

- `python3 scripts/bridge_raw_tester.py --host spa-XXXXXXXXXXXX.local --label set_temp_100f_repeat3 --frame-hex 7e060abf2064c17e --retries 3 --cooldown-ms 2000 --timeout-ms 1800 --out docs/bridge-raw-live-settemp-repeat3.json`

Results:

- All 3 attempts: `ok=true`
- All 3 attempts returned the same `rxHex` frame:
  - `7e 1d ff af 13 00 03 64 08 3b 00 28 64 00 04 08 00 00 02 00 00 00 00 02 02 4c 00 00 02 3a 7e`
- Attempt durations were stable (`5079ms`, `5193ms`, `5223ms`).

Interpretation:

- The bridge response for this `0x20` write is deterministic across repeated attempts in this session.
- Transport variability is low in this run, so remaining uncertainty is likely command semantics/controller behavior rather than socket/bridge reliability.

### 2026-04-28 safe diagnostic set (`0x20`, five variants)

Objective:

- Run five low-risk diagnostics around set-temperature wire semantics and stop early if any variant changed reported setpoint from `0x4c` (`76F`).

Results summary:

1. `baseline_read_status_req` (`7e 08 0a bf 22 00 00 01 58 7e`)
   - Received configuration response (`0x2e`) as expected.
2. `set80_dest0a_chanbf` (`7e 06 0a bf 20 50 4d 7e`)
   - Status response returned `setTemp=0x4c` (`76F`) unchanged.
3. `set80_dest0a_chanaf` (`7e 06 0a af 20 50 ef 7e`)
   - Status response returned `setTemp=0x4c` unchanged.
4. `set80_dest0a_chanbf_pad00` (`7e 07 0a bf 20 50 00 c1 7e`)
   - Status response returned `setTemp=0x4c` unchanged.
5. `set80_destff_chanbf` (`7e 06 ff bf 20 50 00 7e`)
   - Status response returned `setTemp=0x4c` unchanged.

Conclusion:

- No tested `0x20` variant produced a setpoint change; no early stop condition was met.
- Bridge transport and response collection remain healthy; rejection appears semantic/controller-side rather than queue/socket reliability.

### 2026-04-28 safe diagnostic set (`0x20`, ten-test state/timing matrix)

Objective:

- Run 10 low-risk probes with early stop if any status response reported `setTemp != 0x4c` (`76F`).

Read-only sanity probes:

1. `read_config_req` -> response `0x2e` (configuration) OK.
2. `read_filter_req` -> response `0x23` (filter cycles) OK.
3. `read_info_req` -> response `0x24` (information) OK.

Set-temp probes near baseline:

4. `set_77f` (`0x4d`) -> status `0x13`, `setTemp=0x4c`.
5. `set_75f` (`0x4b`) -> status `0x13`, `setTemp=0x4c`.
6. `set_78f` (`0x4e`) -> status `0x13`, `setTemp=0x4c`.
7. `set_74f` (`0x4a`) -> status `0x13`, `setTemp=0x4c`.
8. `set_76f_explicit` (`0x4c`) -> status `0x13`, `setTemp=0x4c`.
9. `set_77f_repeat` (`0x4d`) -> status `0x13`, `setTemp=0x4c`.
10. `set_75f_repeat` (`0x4b`) -> status `0x13`, `setTemp=0x4c`.

Conclusion:

- No test changed reported setpoint from `76F`; early-stop success condition was never reached.
- Read request path is healthy and deterministic; `0x20` writes continue to be ignored/not-applied under current controller semantics/state.

## 2026-04 - Temp-write pause checkpoint (resume later)

Current checkpoint before pivoting away from `0x20`:

- Bridge transport path is validated end-to-end (ingress -> queue -> CTS -> RS485 send -> status reply capture).
- Multiple safe `0x20` frame variants and conservative setpoint values were tested.
- Across all tested runs, status responses remained pinned at `setTemp=0x4c` (`76F`).
- This strongly indicates a controller-semantic acceptance issue (or additional prerequisite) rather than queue/socket reliability.

Resume package for future `0x20` work:

- Last live artifacts:
  - `docs/bridge-raw-live-run.json`
  - `docs/bridge-raw-live-settemp-repeat3.json`
  - `docs/bridge-raw-live-settemp-80f.json`
- Harness and matrix:
  - `scripts/bridge_raw_tester.py`
  - `docs/bridge-raw-command-matrix.example.json`
- Known baseline from status replies:
  - `tempScale=0` (F)
  - reported setpoint byte remains `0x4c` (`76F`)

## 2026-04 - Pivot: Light 1 toggle diagnostics (active)

New active objective:

- Prioritize getting `Spa Light 1` to toggle reliably over bridge-based diagnostics.
- Keep testing low-risk and observable, using the same harness/log discipline.

Initial Light 1 baseline frame:

- Toggle item `0x11` / item code `0x11` payload example from prior work:
  - `7e 07 0a bf 11 11 00 05 7e`

Operating notes for Light 1 phase:

- Continue one-attempt-at-a-time with cooldown and explicit before/after observation windows.
- Record per-attempt frame hex plus observed `light1` state delta.
- Stop early when a repeatable successful toggle path is found.

### 2026-04-28 live Light 1 matrix run (first pass)

Command behavior summary:

- Ran the 4-case Light 1 matrix from `docs/bridge-raw-light1-matrix.example.json` against `spa-XXXXXXXXXXXX.local`.
- All four writes returned immediate status frames (`0x13`) on the bridge socket.
- No case produced an observed Light 1 state transition in returned status payloads.

Cases executed:

1. `7e 07 0a bf 11 11 00 05 7e`
2. `7e 07 0a af 11 11 00 62 7e`
3. `7e 06 0a bf 11 11 61 7e`
4. `7e 07 ff bf 11 11 00 e1 7e`

Observed status pattern:

- Returned status payload byte used by parser for `light1` remained `0x04` (`light1` bit clear) across attempts.
- No `light1` toggle evidence in these responses.

Note on instrumentation:

- Post-write follow-up probe accidentally used a configuration request (`0x22 00`) and therefore returned `0x2e`, which is not a status frame and cannot be used for light-state confirmation.
- The immediate `txRx` responses were valid `0x13` status frames and are sufficient for this pass conclusion.

Conclusion:

- First-pass Light 1 frame-shape variants did not toggle Light 1 in this controller setup.

### 2026-04-28 live Light 1 matrix run (corrected passive status sampling)

Method correction:

- Re-ran Light 1 matrix with passive capture only (no follow-up request injection).
- Parsed only incoming `0x13` status frames and extracted `light1` bit from status payload byte 14 bit 0.

Result:

- All 4 frame variants produced repeated `0x13` status frames over the observation window.
- For every captured status frame, `light1=0` (unchanged).
- No case showed an intra-window transition or post-toggle convergence to `light1=1`.

Conclusion:

- With corrected status sampling, Light 1 still does not toggle for tested destination/channel/pad variants.

Phase-2 trigger (thin HTTP helper endpoint):

- Move from bridge-only to typed HTTP helper when either threshold is met in a rolling 20-attempt window:
  1. `>= 30%` attempts fail due to malformed/incorrectly assembled frames (operator/input errors), or
  2. median case setup time exceeds `90s` because manual hex assembly/interpretation is the bottleneck.

## 2026-04 - Oracle A/B workflow bootstrap

- Added oracle A/B playbook: `docs/oracle-ab-playbook.md`.
- Added starter oracle matrix template: `docs/bridge-raw-oracle-matrix.example.json`.
- Added run-comparison script: `scripts/bridge_raw_compare.py`.

Purpose:

- Use a known-working `balboa_worldwide_app` path as byte-level ground truth.
- Replay equivalent cases via firmware bridge and compare response profiles by case label + frame bytes.
- Convert proven wire/timing deltas into dispatcher changes instead of ad-hoc frame guessing.

### 2026-04-28 source-derived oracle bootstrap (`balboa_worldwide_app` code)

Inputs pulled from upstream source:

- `lib/bwa/messages/toggle_item.rb` confirms `bf 11` with payload `II 00`.
- `lib/bwa/messages/set_target_temperature.rb` confirms `bf 20` with payload `TT`.
- `lib/bwa/client.rb` confirms source `0x0A`, and 100 ms spacing for repeated multi-step toggles.
- `doc/protocol.md` confirms item map and temperature encoding (`TT` doubled in C mode).

Artifacts added:

- `docs/bridge-raw-oracle-derived-matrix.json`
- `docs/bridge-raw-oracle-derived-live-run.json`
- `docs/bridge-raw-oracle-derived-diff-vs-live-frame.json`

Observed from live run against bridge host:

- All source-derived cases were accepted at transport level (`ok=true`).
- `toggle_pump1_item04` frame exactly matched prior live run behavior (`rxHex=""` in both runs).
- Additional `0x11` toggles (`item 0x11`, `0x50`, `0x51`) and near-baseline `0x20` probes (`TT=0x50`, `0x51`) also returned accepted transport with no setpoint convergence evidence.

Current takeaway:

- Frame construction in this firmware is already aligned with upstream `balboa_worldwide_app` for core `0x11`/`0x20` payload shapes.
- Remaining gap is increasingly likely controller-state semantics/prerequisites (mode/range/lock/state timing), not CRC/framing syntax.

## 2026-04 - New test sets from cross-project insights

New evidence incorporated:

- `spaControl` emphasizes explicit bus/session state handling and short repeated toggle pacing.
- Published Balboa notes emphasize half-duplex direction timing and waiting for board-ready opportunities.

Added matrices:

- `docs/bridge-raw-state-precondition-matrix.json`
  - Probes whether toggling `temp_range` / `heating_mode` immediately before `0x20` set-temp affects acceptance/application.
- `docs/bridge-raw-cts-cadence-matrix.json`
  - Probes whether 100 ms repeated `0x11` toggles converge better than single-attempt controls.

Suggested execution:

- `python3 scripts/bridge_raw_tester.py --host <spa-host> --matrix docs/bridge-raw-state-precondition-matrix.json --out docs/bridge-raw-state-precondition-live-run.json`
- `python3 scripts/bridge_raw_tester.py --host <spa-host> --matrix docs/bridge-raw-cts-cadence-matrix.json --out docs/bridge-raw-cts-cadence-live-run.json`

Comparison:

- `python3 scripts/bridge_raw_compare.py --match-by frame --oracle-run docs/bridge-raw-state-precondition-live-run.json --firmware-run docs/bridge-raw-live-run.json --out docs/bridge-raw-state-precondition-diff-vs-baseline.json`
- `python3 scripts/bridge_raw_compare.py --match-by frame --oracle-run docs/bridge-raw-cts-cadence-live-run.json --firmware-run docs/bridge-raw-live-run.json --out docs/bridge-raw-cts-cadence-diff-vs-baseline.json`

### 2026-04-28 live execution results (state-precondition + cadence)

Artifacts:

- `docs/bridge-raw-state-precondition-live-run.json`
- `docs/bridge-raw-state-precondition-diff-vs-baseline.json`
- `docs/bridge-raw-cts-cadence-live-run.json`
- `docs/bridge-raw-cts-cadence-diff-vs-baseline.json`

State-precondition matrix (`temp_range` / `heating_mode` sequencing before `0x20`):

- All attempts returned `ok=true` with immediate status responses.
- Returned status payload remained pinned with setpoint byte `0x4c` (`76F`) across all set-temp probes (`75F`, `80F`, `81F` intents).
- No evidence that toggling `temp_range` or `heating_mode` immediately before set-temp changed write application outcome in this run.

Cadence matrix (100 ms repeated toggles):

- All attempts returned `ok=true`.
- For `light1` and `pump1`, repeated-toggle cadence changed response timing/profile but did not show a clear state-convergence signal in payload fields tracked so far.
- `pump1` triple-toggle captured two distinct status frames over time, but setpoint remained `0x4c` and no decisive command-application transition was observed.

Current implication:

- Transport and bridge acceptance remain healthy under both state-sequencing and short-cadence variants.
- Remaining blocker is still likely controller-specific acceptance semantics beyond currently tested mode/range toggle prep and 100 ms retry cadence.

## 2026-04-28 Light1 deep campaign (no set-temp writes)

Objective:

- Continue testing using recent insights but focus exclusively on `Light1` toggles.
- Probe combined factors: retry cadence, pre-toggle state sequencing, and destination/channel variations.

Artifacts:

- `docs/bridge-raw-light1-deep-matrix.json`
- `docs/bridge-raw-light1-deep-dry-run.json`
- `docs/bridge-raw-light1-deep-live-run.json`
- `docs/bridge-raw-light1-deep-diff-vs-oracle-derived.json`

Cases covered:

- Baseline Light1 (`dest=0x0A`, `chan=0xBF`, `item=0x11`, pad `00`) as single/double/triple (100 ms cadence).
- Pre-sequence with `temp_range` toggle (`item=0x50`) then Light1.
- Pre-sequence with `heating_mode` toggle (`item=0x51`) then Light1.
- Alternate destination/channel variants:
  - `dest=0x0A`, `chan=0xAF`
  - `dest=0xFF`, `chan=0xBF`

Result summary:

- All attempts returned `ok=true` (transport path remains healthy).
- All observed status responses retained Light1-off interpretation (`LF` byte remained `0x04` in captured `0x13` payloads; no `0x03` transition observed).
- 100 ms repeated cadence did not produce a Light1 state transition.
- Pre-toggle state sequencing (`0x50` / `0x51`) also did not produce a Light1 transition.
- Destination/channel variants changed response timing/details but not Light1 state convergence.

Takeaway:

- Light1 no-op persists under broader timing/state/address variants; next diagnostic axis should move from frame permutations toward session/context prerequisites outside current harness (for example, active client/channel ownership interactions).

## 2026-04-28 Light1 session/context campaign

Objective:

- Test session/context ownership hypothesis by injecting RS485 client housekeeping frames before Light1 toggles.

Artifacts:

- `docs/bridge-raw-light1-session-context-matrix.json`
- `docs/bridge-raw-light1-session-context-dry-run.json`
- `docs/bridge-raw-light1-session-context-live-run.json`

Frames injected (from firmware RS485 client patterns):

- `bf 07` ("nothing to send") as `7e050abf077e7e`
- `bf 03` (id/channel ack) as `7e050abf03627e`
- `bf 05 04 37 00` (existing client response style) as `7e080abf05043700ca7e`

Result summary:

- All attempts returned `ok=true` (no transport rejection).
- Post-housekeeping Light1 attempts remained no-op from observed status perspective:
  - returned `0x13` payload kept Light1-off pattern (`LF` byte stayed `0x04` in captured frames).
- Session/context housekeeping injection alone did not unlock Light1 toggle on this controller.

Additional observation:

- Some housekeeping attempts returned `rxHex=""` with longer durations, but subsequent status-bearing attempts still showed unchanged Light1 state.

Takeaway:

- Session-context probe did not surface a successful Light1 path.
- Next likely axis: move from raw bridge injection toward in-loop firmware timing where command enqueue is tied to immediate CTS edge plus richer before/after status sampling windows.

## 2026-04-28 CTS-edge instrumentation (firmware endpoint)

Implemented:

- Added RS485 CTS counters/telemetry:
  - `lastCtsMs`, `ctsCount`
  - `nextCtsArmCount`, `nextCtsFireCount`
- Added "arm on next CTS" raw-frame path in RS485 loop.
- Added web diagnostic endpoint:
  - `GET /api/diag/light1_next_cts`
  - Arms a Light1 toggle (`item 0x11`) for immediate transmit on the *next* CTS event, then observes result window.

Endpoint query params:

- `observe_ms` (default `2500`, clamp `200..12000`)
- `dest=wifi|id` (default `wifi`)
- `pad=00|none` (default `00`)

Response highlights:

- `armedAt` / `firedAt` counters
- `before` + `after` snapshots (`light1`, pumps, CTS counters, queue depth)
- `fired` and `waitElapsedMs`
- `light1Changed`

Planned test loop after flashing:

1. Baseline: `GET /api/status/controls`
2. Trial: `GET /api/diag/light1_next_cts?observe_ms=4000`
3. Repeat 10x and record:
   - `fired`, `light1Changed`, `before.light1`, `after.light1`
4. A/B variants:
   - `dest=id`
   - `pad=none`
   - larger `observe_ms` (e.g., `7000`)

### 2026-04-28 live CTS-edge endpoint trials (post-OTA)

Firmware:

- `version=1.6.1`
- `build=Apr 28 2026 - 12:16:38`
- `restartReason=Software reset via esp_restart - OTA Update`

Artifact:

- `docs/diag-light1-next-cts-live-run.json` (local, gitignored; shape: [`diag-light1-next-cts-live-run.example.json`](diag-light1-next-cts-live-run.example.json))

Trial groups and outcomes:

1. `next_cts_default` (`observe_ms=4000`) x10
   - `ok=10`, `fired=10`, `light1Changed=0`
2. `next_cts_dest_id` (`observe_ms=4000`, `dest=id`) x6
   - `ok=6`, `fired=6`, `light1Changed=0`
3. `next_cts_pad_none` (`observe_ms=4000`, `pad=none`) x6
   - `ok=6`, `fired=6`, `light1Changed=0`
4. `next_cts_dest_id_pad_none` (`observe_ms=5000`, `dest=id`, `pad=none`) x6
   - `ok=6`, `fired=6`, `light1Changed=0`

Interpretation:

- The strict readiness-edge hypothesis is now explicitly tested:
  - commands were armed and fired on next CTS as designed (`fired=true` every trial).
- Even with confirmed CTS-edge execution, Light1 still did not transition in any trial.
- This narrows likely root cause away from "missing ready-edge timing" and further toward controller-specific command acceptance semantics outside currently known `0x11` light toggle behavior.

### 2026-04-28 decoded-byte CTS-edge trials (post-instrumentation update)

Firmware:

- `version=1.7.0`
- `build=Apr 28 2026 - 12:42:13`
- `restartReason=Software reset via esp_restart - OTA Update`

Artifact:

- `docs/diag-light1-next-cts-live-run-decoded.json` (local, gitignored)

Trial groups:

1. `default` (`observe_ms=4000`) x3
2. `dest_id` (`observe_ms=4000`, `dest=id`) x2
3. `pad_none` (`observe_ms=4000`, `pad=none`) x2

Outcome:

- `fired=true` across successful trials.
- `light1Changed=0` in all successful trials.
- Decoded status-byte snapshots (`hf`, `pp`, `lf`, `stRaw`) now included in each before/after payload for direct delta analysis.

Interpretation:

- Even with explicit CTS-edge firing and structured byte-level before/after decode in the endpoint, Light1 still shows no state transition.
- This further de-risks timing-only explanations and reinforces controller-specific semantic mismatch hypotheses for this command family.

### 2026-04-28 rolling window CTS-edge trials (`light1_next_cts_window`)

Firmware behavior exercised:

- Endpoint: `GET /api/diag/light1_next_cts_window`
- Run params: `observe_ms=7000`, `sample_ms=120`
- Live artifact: `docs/diag-light1-next-cts-window-live-run.json` (local, gitignored)

Observed results:

- 8/8 HTTP calls succeeded; 7/8 returned full structured payloads with rolling samples.
- In each complete payload, arming and fire counters advanced by exactly one (`fireMax = fireBefore + 1`), confirming next-CTS execution.
- Across all sampled windows, `light1` remained `0` and decoded light-field byte `lf` stayed constant at `0x04` (no transient flips detected).

Interpretation:

- Rolling multi-sample capture did not reveal short-lived state transitions that point-snapshot endpoints might miss.
- This further reduces the probability of a timing-window-only issue and keeps focus on controller-specific write acceptance semantics / ownership context outside current `0x11` assumptions.

### 2026-04-28 rolling-window A/B variants (`dest`, `pad`, pre-housekeeping)

Artifact:

- `docs/diag-light1-next-cts-window-ab-live-run.json` (local, gitignored)

Matrix:

- `default`
- `dest=id`
- `pad=none`
- `dest=id&pad=none`
- `pre_bf03_then_default` (inject `7e050abf03627e` to bridge before endpoint call)

Result summary:

- Stable variants (`default`, `dest=id`, `pad=none`) each returned 3/3 complete sampled windows with `result=armed_next_cts` and observed fire-counter advancement.
- Across all complete sampled windows in those variants, `light1` never reached `1`, and `lf` remained fixed (`0x04` only, no transitions).
- Mixed variants (`dest=id&pad=none`, `pre_bf03_then_default`) showed intermittent HTTP/output instability (partial read / timeout / empty object) but still showed no Light1 or `lf` transition in successful sampled runs.

Interpretation:

- A/B parameters did not produce any evidence of accepted/apply behavior for Light1.
- The remaining evidence points away from simple destination/pad/CTS-edge timing differences and toward controller-specific command semantics and/or client-ownership constraints not yet modeled.

### 2026-06-22 fault log decode + reminder text (tub validation checklist)

Firmware change (not a command-write experiment): decode **`0x28`** fault-log responses, request via **`0x22`/`0x20`/`0xFF`** in **`configurationRequest()`**, and expose **`reminderText`** on MQTT/**`/status`**.

**Tub checklist (pending live run):**

1. Serial log after boot: configuration batch includes **`FaultLog`** when fault data is stale.
2. **`GET /api/config/export`** → **`snapshot.faultLog`**: **`faultCode`**, **`faultMessage`**, **`faultLogTime`** populated (or **`None`** when no stored fault).
3. **`/config`** → Other spa datasets → fault log shows decoded KV rows + raw hex collapsible.
4. **`/status`** → Panel and flags → **Reminder** row shows **`None`** or matches panel message.
5. MQTT: **`Spa/<gateway>/faultLog/faultMessage`**, **`…/status/reminderText`** update after spa traffic.
6. HA: rediscover; diagnostic entities **`fault_code`**, **`fault_message`**, **`fault_log_time`**, **`reminder`** appear.

**Bench note:** [`emulator/spaEmulator.js`](../emulator/spaEmulator.js) does not simulate **`0x28`**; fault decode needs tub traffic or injected frame.

