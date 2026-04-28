# Command Write Debug Log

This log tracks attempted command-write solutions and measured outcomes so we do not duplicate experiments.

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

Phase-2 trigger (thin HTTP helper endpoint):

- Move from bridge-only to typed HTTP helper when either threshold is met in a rolling 20-attempt window:
  1. `>= 30%` attempts fail due to malformed/incorrectly assembled frames (operator/input errors), or
  2. median case setup time exceeds `90s` because manual hex assembly/interpretation is the bottleneck.
