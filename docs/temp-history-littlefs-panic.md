# Temperature history LittleFS panic (parked)

**Status (2026-07-28):** Flash **persist is off by default** (`TEMP_HISTORY_FLASH_PERSIST` = 0 in [`src/main.h`](../src/main.h)). RTC sampling and the 24h chart/MQTT/ePaper path still run. Re-enable persist only after isolation below.

Related code: [`lib/tempHistory/`](../lib/tempHistory/).

## Product feature

| Piece | Role | Still on? |
|-------|------|-----------|
| RTC buffer (144 × 10 min = 24h) | Live chart / MQTT `temperatureHistory` / ePaper | **Yes** |
| Soft-reboot retention (RTC_NOINIT) | History survives panic/OTA soft reset | **Yes** |
| LittleFS `/TempHist.bin` hourly write | Survive **power loss** | **No (parked)** |
| Load `/TempHist.bin` at boot | Restore after power loss | **No (parked)** — gated with persist; cold boot resets RTC buffer |

Portal copy: history is in RAM (soft reboot OK; power loss clears until it refills).

## Symptom

- `ESP_RST_PANIC` about **1 hour after boot** (production `TEMP_FLASH_SAVE_MIN_MS`), or **~5 min** with temporary 5-minute persist interval builds.
- Fault ring: crumbs reach **before** LittleFS write work; **never** a successful “after save” while persist was enabled.
- Not caused by BWA phone / TCP 4257 (crash with phone disconnected).
- Not caused by Balboa stale config refresh alone (fingerprint: crash at 5m temp persist, stale gate at 8m never reached).

## What we proved

| Experiment | Result |
|------------|--------|
| Fingerprint: temp persist 5m, stale refresh gated 8m | Panic at ~5.01m at tempHist save; no 8m gate |
| FS mutex + “safer” tmp/rename save; assets returning 503 | Still panic at ~5m; crumb `tempHist before save … heap≈125772` |
| Persist disabled (RTC samples only) | Stable past 5m and 10m |
| Dry-run: alloc+pack only, no LittleFS | Stable; crumbs `dry alloc ok` / `pack ok bytes=588` |
| Heap at crash | ~125 KB free → not ordinary heap exhaustion |

**Conclusion:** Necessary trigger is the **temp-history LittleFS mutation path** (not the timer alone, not pack/alloc alone, not AsyncWebServer as sole cause). Lead hypotheses: LittleFS write/metadata, partition pressure/GC, or path-specific corruption around `/TempHist.bin` (or a `.tmp`).

## What did not work / do not repeat blindly

- Claiming “missing `:33` wall clock” proved the overflow fix (that was `STALE_TIME` phase after OTA). The overflow fix in `configurationRequest()` is real and separate; it did **not** stop this remaining ~hourly panic while persist was on.
- Full-file heap materialize of SPA assets under an FS lock (≈85 KB `main.js`) → HTTP **503** when `maxAlloc` tight; worse for soaks. Do not resurrect that approach.
- A single `before save` crumb (too coarse: covered alloc → remove → open → write → flush → close).
- Assuming an FS mutex alone fixed the panic (it did not). That experimental `lib/fsLock/` + chunked locked static serve was **reverted** when parking persist; reintroduce only if a later resume needs it.
- Auto-`LittleFS.format()` in firmware (would wipe SPA + `.env`; only deliberate maintenance + `uploadfs`).

## Tree state while parked

- Save and load are both compile-gated behind `TEMP_HISTORY_FLASH_PERSIST` (default **0**). Cold/invalid RTC buffer resets in RAM; `/TempHist.bin` is not opened. Flipping to **1** restores the original load + truncate-write path (write known to panic on the tub).
- Temporary soak helpers (`DIAG_TEMPHIST_FAST_PERSIST`, `DRY_RUN`, `PROBE_NEWFILE`, fine-grained fault crumbs, tmp→verify→rename) were **removed from the tree** after the investigation. Re-add them from this resume plan when circling back — do not assume they still exist in source.

## Recommended resume sequence

1. Unique `VERSION` string per probe OTA.
2. Temporarily set a **5-minute** persist interval for faster soaks.
3. Dry-run @5m: heap-alloc + pack only, **no** LittleFS calls (already passed once — re-check after large FS changes).
4. Probe write to a **never-before-used** filename (e.g. `/TempHistProbe.v2.bin`; bump suffix if that file already exists on-device).
5. Fine-crumb real save (tmp → verify → final remove → rename); last crumb must name the op that never completes.
6. If probe OK but `/TempHist.bin` replace fails: **new versioned filename**, ignore old file — do **not** delete old on every boot; no auto-format.
7. If **any** LittleFS write panics: move blob to **NVS/Preferences**, or keep persist off. Note: Analytics still writes `.bin` files on day rollover/reset — useful contrast if TempHist is unique.

Optional hardening to consider again only after a write path is proven safe: recursive FS mutex + per-chunk locked static reads (avoid full-file heap materialize).

## Field timeline (abbrev.)

- Pre-fix: hourly panic; overflow batching P0 fixed separately (`configurationRequest` one-frame-per-queue) in 2.21.0.
- 2026-07-28 evening: fingerprint → 5m tempHist panic; FS-lock OTA still panic; nopersist stable; dry-run stable.
- Parked with persist default off; diag scaffolding stripped from tree.

Local review scratch (gitignored): `docs/local/reviews/cursor-temphist-lfs-fix-triage.md`, `codex-temphist-lfs-fix-review-2026-07-29.md`, residual hourly panic reviews.
