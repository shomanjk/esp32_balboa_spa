# RS485 5-minute diagnostic checklist

Use this flow when command writes look healthy but spa state does not change.

## 1) Prepare one clean capture window (30-60s)

- Keep only one active test action (for example one Light1 toggle test series).
- Avoid repeating the same pasted block in notes; save raw logs once.
- If possible, reduce MQTT discovery noise temporarily to improve RS485 signal visibility.

## 2) Run one controlled command burst (60-120s)

- Trigger your existing test path (bridge harness or `/api/diag/light1_next_cts`) for a small fixed count.
- Keep parameters fixed for this pass (no destination/pad changes mid-capture).
- Record the exact command frame under test.

## 3) Triage the captured logs automatically (30s)

Save logs to a text file, then run:

`python3 scripts/diag_log_triage.py --in <path-to-log.txt> --out docs/diag-last-triage.json`

What to look at in output:

- `counts.rs485_sent` > 0 confirms firmware sent frames.
- `counts.invalid_length` > 0 indicates receive corruption/misalignment.
- `counts.ha_discovery_publish_failed` > 0 indicates MQTT/HA instability.
- `duplicateLineCount` > 0 means pasted capture likely contains repeated blocks.

## 4) Decide next axis (60s)

- If `invalid_length` appears: prioritize RS485 physical/timing checks before more frame variants.
- If MQTT publish failures dominate: stabilize broker/session first, then re-run.
- If sends are clean and no invalid frames: continue controller-semantic testing.

## 5) Archive artifacts (30s)

- Save raw log text in your local notes.
- Keep machine-readable summary in `docs/diag-last-triage.json` (or dated variant).
- Add a short outcome line to `docs/command-write-debug-log.md` only if this run changed your hypothesis.
