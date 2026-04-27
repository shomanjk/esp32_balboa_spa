---
name: ESP32 Robustness Hardening
overview: Define a reliability-first hardening roadmap for tub-installed ESP32 firmware, prioritizing concrete failure handling, command-path completion, and observable degraded states without over-engineering fleet-style controls.
todos:
  - id: backpressure-input-hardening
    content: Harden queue ownership, malformed input handling, and low-memory/backpressure behavior
    status: pending
  - id: command-path
    content: Complete shared MQTT and web command execution pipeline with validation and acknowledgements
    status: pending
  - id: health-snapshot
    content: Add lightweight health snapshot telemetry and machine-readable health endpoint
    status: pending
  - id: recovery-ladder
    content: Implement bounded staged recovery for Wi-Fi, MQTT, RS485, and controlled reboot protection
    status: pending
  - id: ota-safety
    content: Tighten OTA production defaults and post-update health verification workflow
    status: pending
  - id: observability
    content: Standardize fault telemetry and verification workflows for fault injection and soak testing
    status: pending
isProject: false
---

# ESP32 Reliability Hardening Plan

## Reliability Objectives

- Ensure the tub-side device self-recovers from realistic field failures without requiring physical access for common issues.
- Make degraded behavior observable remotely over MQTT and HTTP with clear freshness and fault signals.
- Reduce the risk of crashes, reboot loops, memory leaks, and misleading command acknowledgements.
- Preserve a simple operating model appropriate for one-off or small-number tub-side installs.

## Current Baseline (already present)

- Task watchdog is initialized in [`src/main.ino`](/Users/jerrodkogut/Documents/Dev Projects/esp32_balboa_spa/src/main.ino) and reset during active message processing in [`lib/spaMessage/spaMessage.cpp`](/Users/jerrodkogut/Documents/Dev Projects/esp32_balboa_spa/lib/spaMessage/spaMessage.cpp) and OTA progress in [`lib/wifiModule/wifiModule.cpp`](/Users/jerrodkogut/Documents/Dev Projects/esp32_balboa_spa/lib/wifiModule/wifiModule.cpp).
- Spa data structures already track `lastUpdate` / `lastRequest`, and stale detection is already used in [`lib/spaMessage/spaMessage.cpp`](/Users/jerrodkogut/Documents/Dev Projects/esp32_balboa_spa/lib/spaMessage/spaMessage.cpp).
- Basic node telemetry already publishes over MQTT in [`lib/mqttModule/mqttModule.cpp`](/Users/jerrodkogut/Documents/Dev Projects/esp32_balboa_spa/lib/mqttModule/mqttModule.cpp).
- OTA support, auth options, timeout controls, and OTA lifecycle logging already exist in [`lib/wifiModule/wifiModule.cpp`](/Users/jerrodkogut/Documents/Dev Projects/esp32_balboa_spa/lib/wifiModule/wifiModule.cpp).
- Restart reason persistence already exists in [`lib/restartReason/restartReason.cpp`](/Users/jerrodkogut/Documents/Dev Projects/esp32_balboa_spa/lib/restartReason/restartReason.cpp).
- OTA-capable partitioning is already configured in [`spa_module.csv`](/Users/jerrodkogut/Documents/Dev Projects/esp32_balboa_spa/spa_module.csv).

## Key Gaps To Close

- MQTT command callback currently echoes payloads instead of dispatching validated commands in [`lib/mqttModule/mqttModule.cpp`](/Users/jerrodkogut/Documents/Dev Projects/esp32_balboa_spa/lib/mqttModule/mqttModule.cpp).
- Web `device_request` handling parses requests but does not execute control actions in [`lib/spaWebServer/spaWebServer.cpp`](/Users/jerrodkogut/Documents/Dev Projects/esp32_balboa_spa/lib/spaWebServer/spaWebServer.cpp).
- Queue overflow paths log dropped messages but need explicit ownership cleanup, counters, and telemetry in [`lib/spaMessage/spaMessage.cpp`](/Users/jerrodkogut/Documents/Dev Projects/esp32_balboa_spa/lib/spaMessage/spaMessage.cpp) and [`lib/localRS485Communication/rs485.cpp`](/Users/jerrodkogut/Documents/Dev Projects/esp32_balboa_spa/lib/localRS485Communication/rs485.cpp).
- Web XML parsing assumes request structure exists and should be hardened against malformed or partial input in [`lib/spaWebServer/spaWebServer.cpp`](/Users/jerrodkogut/Documents/Dev Projects/esp32_balboa_spa/lib/spaWebServer/spaWebServer.cpp).
- Health visibility is spread across stale-data checks and ad hoc node status topics; there is no single machine-readable health summary.
- Recovery behavior is mostly implicit today: Wi-Fi reconnect and MQTT reconnect exist, but there is no bounded escalation policy tied to sustained subsystem failure.
- OTA is usable, but post-update success criteria and production guidance should be made explicit rather than fleet-style staged rollout.

## Implementation Phases

### Phase 1: Backpressure and Input Hardening (highest priority)

- Fix queue overflow ownership so dropped queued messages are freed correctly and counted.
- Add monotonic counters for queue-full events, malformed XML requests, malformed RS485 frames, CRC failures, and reconnect attempts.
- Add null checks and parse-failure handling for XML request parsing in the web handler.
- Add lightweight low-heap and queue-depth reporting to help distinguish logic faults from memory pressure.
- Prefer simple failure containment over recovery abstraction in this phase.

### Phase 2: Control Path Completion

- Implement a single shared command executor used by both MQTT and web requests.
- Flow: command ingestion -> validation -> Balboa frame build/enqueue -> acknowledgement publication/response.
- Define acknowledgements carefully:
- `accepted` means the command was parsed and queued successfully.
- `applied` means the resulting spa state was later observed in status data.
- `rejected` means validation or enqueue failed.
- Add rate limiting / dedupe guards to avoid repeated command storms from retries or UI polling.
- Keep protocol-specific ingestion thin; put command semantics in one place.

### Phase 3: Lightweight Health Snapshot

- Add a small health module or `HealthSnapshot` helper rather than a full central `HealthManager` state machine.
- Compute health from existing timestamps/counters:
- spa status freshness,
- RS485 receive activity,
- Wi-Fi connected state,
- MQTT connected state and last successful publish/report time,
- queue depth / queue overflow counters,
- free heap watermark if practical.
- Expose a dedicated `/api/health` JSON endpoint rather than overloading `/api/version`.
- Publish matching MQTT health/fault topics for remote automation.
- Use simple levels such as `healthy`, `degraded`, and `failed`, but keep logic table-driven and minimal.

### Phase 4: Recovery Ladder

- Implement bounded recovery steps only for subsystems that already have a meaningful lifecycle:
- Wi-Fi reconnect cycle with retry counters and cooldown,
- MQTT reconnect retry with bounded retry cadence,
- RS485 port re-init when spa data is stale beyond threshold,
- controlled full reboot only after sustained failure.
- Add reboot-loop protection using RTC/NVS counters and time windows.
- Avoid adding synthetic "reset web server state" or other resets that the current architecture does not naturally support.
- Keep recovery policy conservative so a transient network issue does not trigger reboot storms.

### Phase 5: OTA Safety

- Keep OTA auth enabled by default for production-facing guidance, while preserving trusted-LAN development flexibility.
- Add post-update health verification: mark update successful in telemetry only after the device reaches a stable health window after reboot.
- Document actual rollback/fallback behavior based on the current dual OTA partition layout.
- Prefer documented recovery and validation over fleet-style staged rollout unless deployment scope grows materially.

### Phase 6: Observability and Verification

- Standardize fault/health topic names and JSON payload fields.
- Define a small fault taxonomy suitable for automation and log triage, for example:
- `FAULT_RS485_STALE`
- `FAULT_QUEUE_FULL`
- `FAULT_XML_PARSE`
- `FAULT_WIFI_DISCONNECTED`
- `FAULT_MQTT_DISCONNECTED`
- `FAULT_LOW_HEAP`
- Separate verification into:
- automatable tests: malformed HTTP/XML input, queue pressure, command validation, stale-state calculations,
- bench/manual tests: unplug RS485, drop Wi-Fi AP, stop MQTT broker, long soak test.
- Run a 24-72 hour soak test and confirm bounded reboot count, stable heap trend, and expected stale/recovery behavior.

## Verification Strategy

- Unit or harness-level verification where practical for command validation, ack semantics, malformed request handling, and health calculations.
- Bench fault injection:
- unplug RS485,
- stop MQTT broker,
- power-cycle AP or remove Wi-Fi coverage,
- force queue pressure,
- submit malformed XML and partial request bodies.
- Confirm expected transitions from healthy -> degraded -> failed and verify recovery cooldown behavior.
- Verify command acks are not reported as successful unless the command was either accepted or actually observed as applied, per the defined semantics.

## Key Files To Modify

- Core loop/orchestration: [`src/main.ino`](/Users/jerrodkogut/Documents/Dev Projects/esp32_balboa_spa/src/main.ino)
- RS485 hooks and counters: [`lib/localRS485Communication/rs485.cpp`](/Users/jerrodkogut/Documents/Dev Projects/esp32_balboa_spa/lib/localRS485Communication/rs485.cpp)
- Message freshness, queue handling, and command enqueue: [`lib/spaMessage/spaMessage.cpp`](/Users/jerrodkogut/Documents/Dev Projects/esp32_balboa_spa/lib/spaMessage/spaMessage.cpp)
- MQTT command and health topics: [`lib/mqttModule/mqttModule.cpp`](/Users/jerrodkogut/Documents/Dev Projects/esp32_balboa_spa/lib/mqttModule/mqttModule.cpp)
- Wi-Fi/OTA recovery and policy: [`lib/wifiModule/wifiModule.cpp`](/Users/jerrodkogut/Documents/Dev Projects/esp32_balboa_spa/lib/wifiModule/wifiModule.cpp)
- Web command parsing and health endpoint: [`lib/spaWebServer/spaWebServer.cpp`](/Users/jerrodkogut/Documents/Dev Projects/esp32_balboa_spa/lib/spaWebServer/spaWebServer.cpp)
- Production defaults/docs: [`src/config-example.h`](/Users/jerrodkogut/Documents/Dev Projects/esp32_balboa_spa/src/config-example.h), [`README.md`](/Users/jerrodkogut/Documents/Dev Projects/esp32_balboa_spa/README.md), [`CHANGELOG.md`](/Users/jerrodkogut/Documents/Dev Projects/esp32_balboa_spa/CHANGELOG.md), [`AGENTS.md`](/Users/jerrodkogut/Documents/Dev Projects/esp32_balboa_spa/AGENTS.md)

## Notes On Scope

- This plan is intentionally optimized for the current tub-side ESP32 deployment model, not a large managed fleet.
- Prefer small, composable telemetry and recovery helpers over introducing a heavyweight central supervisor too early.
- If the project later expands to many deployed devices with coordinated remote updates, revisit staged rollout and stronger rollback orchestration then.
