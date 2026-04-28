# Oracle A/B Command Validation Playbook

This playbook turns a known-working `balboa_worldwide_app` command path into a byte-level oracle for this firmware.

Goal: determine exactly what differs between "command accepted and applied" versus "command accepted but ignored".

## 1) Capture a known-good oracle run

1. Use the same spa hardware and a command path that is confirmed to work (for example: `balboa_worldwide_app` + MQTT/Home Assistant entity action).
2. Capture raw command/response bytes for a short test set:
   - Light 1 toggle
   - Pump 1 toggle
   - Set temperature (small delta, low risk target like 80F)
3. Save the resulting frame list in a matrix JSON modeled after:
   - `docs/bridge-raw-oracle-matrix.example.json`

Keep per-command metadata if available: target, channel byte, payload length, retry spacing, and timing between repeated writes.

## 2) Replay the same matrix against this firmware bridge

Run against the ESP bridge host:

`python3 scripts/bridge_raw_tester.py --host <spa-host> --matrix docs/bridge-raw-oracle-matrix.example.json --out docs/bridge-raw-firmware-run.json`

If you have the oracle run in bridge_raw_tester format, store it as:

- `docs/bridge-raw-oracle-run.json`

## 3) Compare oracle vs firmware behavior

Use the comparer:

`python3 scripts/bridge_raw_compare.py --oracle-run docs/bridge-raw-oracle-run.json --firmware-run docs/bridge-raw-firmware-run.json --out docs/bridge-raw-oracle-diff.json`

Interpretation:

- `match=true`: same high-level response profile (rx payloads + success state).
- `okMatch=true` but `rxMatch=false`: transport likely fine; semantics differ.
- case present only in oracle: matrix drift or missing test coverage.

## 4) Apply findings to command builder

Prioritize deltas in this order:

1. destination byte and channel byte
2. payload shape (especially optional/pad bytes)
3. timing/retry spacing
4. status preconditions (mode/range/lock state before command)

Translate proven deltas into:

- `lib/spaMessage/spaCommandDispatcher.cpp`
- web and MQTT command call paths that feed dispatcher

## 5) Record each pass

Append each run to:

- `docs/command-write-debug-log.md`

Include exact frame bytes tested and resulting status bytes so subsequent passes avoid duplicate experiments.
