# Hardware targets and bring-up checklist

This page lists **supported** boards (matching a checked-in PlatformIO **`[env:…]`**) and **planned / community bring-up** targets. It is **not** a substitute for release notes: shipped behavior stays documented in the [README](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/README.md) and [CHANGELOG](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/CHANGELOG.md).

**Verified “I ran this tub + board” reports** still belong on **[Hardware field notes](Hardware-field-notes)**.

---

## Status overview

| Board / stack | MCU / flash (typical) | PlatformIO env | Status |
|---------------|------------------------|----------------|--------|
| Generic ESP32 dev board (tub-side RS485) | ESP32 · varies | `ESP32ota`, `ESP32prodOta`, etc. | **Supported** |
| **M5 Atom Lite** + Atomic RS485 Base | ESP32-PICO-D4 · 4 MB | `M5AtomLite-tub`, `M5AtomLite-tub-ota` | **Supported** ([README — M5 section](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/README.md#m5-atom-lite--atomic-rs485-base-tub-side)) |
| LilyGo T5 ePaper | ESP32-S3 · varies | `ESP32-epd47` | **Supported** (remote display; `REMOTE_CLIENT` + `spaEpaper`) |
| **M5 AtomS3 Lite** + *(RS485 wiring TBD)* | ESP32-S3FN8 · 8 MB | *(none yet)* | **Planned** — see below |

---

## Bring-up checklist (copy for new boards)

Use this when adding or validating hardware. Paste into an Issue or a field-notes row when done.

```text
Board / SKU:
Role (tub-side RS485 / remote client / …):
PlatformIO env name (existing or proposed):
platformio.ini — board =
platformio.ini — extra build_flags (USB CDC, chip define, …):
Partitions: spa_module.csv OK? (resize for larger flash?)
src/config.h — TX485_Rx / TX485_Tx / AUTO_TX:
UART port (default Serial2 in this project):
Optional DE/RE or transceiver wiring notes:
Mechanical stack (e.g. M5 Atom on Atomic RS485 Base): verified compatible? doc link?
Optional RGB / status LED: enabled build flag (e.g. M5_ATOM_LED)? library (M5Atom vs M5Unified vs FastLED only)?
Libraries added to env vs [com]: lib_deps = …
USB upload / bootloader / CDC quirks:
Smoke test: serial log, /status, RS485 counters (/api/rs485):
Firmware version (/api/version):
```

**Tub-side compile-time feature flags** used by shipped gateway envs mirror [`platformio.ini`](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/platformio.ini): typically **`LOCAL_CLIENT`**, **`LOCAL_CONNECT`**, and **`BRIDGE`** (plus shared **`PRODUCTION`** / logging). The RS485 framing and MQTT stack do not depend on which ESP32 variant you pick; **pins, bootloader, partitions, and M5 libs** do.

---

## Planned: M5 AtomS3 Lite

Official hardware docs:

- [M5 AtomS3 Lite](https://docs.m5stack.com/en/core/AtomS3%20Lite) (ESP32-S3, 8 MB flash, HY2.0 / GPIO layout differs from Atom Lite)
- Compare: [M5 Atom Lite](https://docs.m5stack.com/en/core/ATOM%20Lite) (**supported** tub-side stack today)

### Blocked on / do not assume

1. **RS485 UART GPIOs:** The supported **Atom Lite on [Atomic RS485 Base](https://docs.m5stack.com/en/atom/Atomic%20RS485%20Base)** pairing uses firmware **`TX485_Rx` 22 / `TX485_Tx` 19** (see [`src/config-example.h`](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/src/config-example.h)). **Do not** copy those pins to AtomS3 until M5 documentation (or your own probing) confirms the **same stack** routes UART to the same lines for the S3 module — HY2.0/header pins differ between products.
2. **`config-example.h`:** Avoid adding unverified pin blocks there; misleading comments are worse than “TBD on wiki.”
3. **Optional status LED:** Today’s tub-side RGB path is **`M5_ATOM_LED`** with [`m5stack/M5Atom`](https://registry.platformio.org/libraries/m5stack/M5Atom) and [`src/led_control.cpp`](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/src/led_control.cpp). AtomS3-class boards are typically brought up with **M5Unified** in M5’s own PlatformIO examples; porting or gating LED code is **deferred** until a maintainer-owned device is on the bench.

### Partition / flash size

[`spa_module.csv`](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/spa_module.csv) is **4 MB–oriented** (fits Atom Lite). An **8 MB** AtomS3 Lite can usually use the **same** CSV (unused flash at the end). Resizing app or LittleFS is **optional** future work if you need more on-chip space.

### Deferred implementation (when promoting to “Supported”)

1. Add PlatformIO **`[env:…]`** entries (e.g. `M5AtomS3-tub` / `-ota`) with an ESP32-S3 **`board`**, USB CDC flags as needed, **`lib_deps`** appropriate for LED choice, and the same tub-side **`build_flags`** as `M5AtomLite-tub` where applicable.
2. **`led_control`:** `#ifdef`/split translation unit — **M5Unified** vs legacy **M5Atom**, or omit **`M5_ATOM_LED`** until ported.
3. **`spa_module.csv`:** optional larger partitions for 8 MB parts.
4. Update [README](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/README.md), [`AGENTS.md`](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/AGENTS.md), and **[CHANGELOG](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/CHANGELOG.md)** when an env merges.

---

## Wiki publishing

Repo copies under **`wiki/`** sync to GitHub Wiki via CI (see [`wiki/BOOTSTRAP.md`](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/wiki/BOOTSTRAP.md) and `.github/workflows/publish-wiki.yml`).

---

 [← Wiki home](Home) · [Getting started](Getting-started) · [Hardware field notes](Hardware-field-notes)
