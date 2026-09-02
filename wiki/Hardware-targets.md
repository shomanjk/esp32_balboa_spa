# Hardware targets and bring-up checklist

This page lists **supported** boards (matching a checked-in PlatformIO **`[env:…]`**) and **bring-up** targets. It is **not** a substitute for release notes: shipped behavior stays documented in the [README](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/README.md) and [CHANGELOG](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/CHANGELOG.md).

**Verified “I ran this tub + board” reports** still belong on **[Hardware field notes](Hardware-field-notes)**.

---

## Status overview

| Board / stack | MCU / flash (typical) | PlatformIO env | Status |
|---------------|------------------------|----------------|--------|
| Generic ESP32 dev board (tub-side RS485) | ESP32 · varies | **`ESP32usb`** (first USB), **`ESP32ota`** / **`ESP32prodOta`** (OTA) | **Supported** — pins in `config.h` |
| **M5 Atom Lite** + Atomic RS485 Base | ESP32-PICO-D4 · 4 MB | `M5AtomLite-tub`, `M5AtomLite-tub-ota` | **Supported** — env pins **RX 22 / TX 19** ([README — M5 section](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/README.md#m5-atom-lite--atomic-rs485-base-tub-side)) |
| **M5 Atom Lite** + alternate RS485 ([Tail485](https://docs.m5stack.com/en/atom/tail485), [Unit RS485](https://docs.m5stack.com/en/unit/rs485), …) | ESP32-PICO-D4 · 4 MB | `M5AtomLite-tub` + **`#undef` / 32/26 in `config.h`** | **Alternate** — [32/26 wiring](#atom-lite--alternate-rs485-3226-pins); issue #31 unverified on v2.28+ |
| **M5 AtomS3 Lite** + Atomic RS485 Base (**not** display AtomS3) | ESP32-S3FN8 · 8 MB | `M5AtomS3Lite-tub`, `M5AtomS3Lite-tub-ota` | **Bring-up** — env pins **RX 5 / TX 6**; RGB via FastLED GPIO **35** |
| LilyGo T5 ePaper | ESP32-S3 · varies | `ESP32-epd47` | **Supported** (remote display; `REMOTE_CLIENT` + `spaEpaper`) |

---

## RS485 pin ownership

| Env class | Who sets `TX485_Rx` / `TX485_Tx` / `AUTO_TX` |
|-----------|-----------------------------------------------|
| **`M5AtomLite-tub`**, **`M5AtomLite-tub-ota`** | PlatformIO `build_flags` (**22 / 19**, `AUTO_TX`) — omit overrides in `config.h` |
| **`M5AtomS3Lite-tub`**, **`M5AtomS3Lite-tub-ota`** (AtomS3 Lite) | PlatformIO `build_flags` (**5 / 6**, `AUTO_TX`) — omit overrides in `config.h` |
| Generic (`ESP32usb`, `ESP32ota`, …) | [`src/config.h`](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/src/config-example.h) (`#ifndef`-guarded defaults **16 / 17**) |

[`src/config-example.h`](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/src/config-example.h) wraps pin/`AUTO_TX` defines in `#ifndef` so env `-D` wins **when you omit overrides**. The alternate **32/26** block uses **`#undef` then `#define`**, which **does** override env pins on `M5AtomLite-tub` (see [alternate RS485](#atom-lite--alternate-rs485-3226-pins)). **Migration:** private `config.h` files that still `#define TX485_*` / `AUTO_TX` **unconditionally** will redefinition-warn/error on M5 envs until those lines are wrapped or removed.

Nonstandard RS485 on **Atom Lite** (e.g. [Tail485](https://docs.m5stack.com/en/atom/tail485) tail stack or [Unit RS485](https://docs.m5stack.com/en/unit/rs485) Grove at **32/26**): stay on **`M5AtomLite-tub`** and enable the **`#undef` / 32/26 block** in `config.h`. **`ESP32usb`** / **`ESP32ota`** are for **non-M5 ESP32 dev boards** (`board = esp32dev`) — not Atom Lite.

---

## Atom Lite + alternate RS485 (32/26 pins)

The [M5 Atom Lite](https://docs.m5stack.com/en/core/ATOM%20Lite) uses the **ESP32-PICO-D4** (4 MB flash). Issues or posts that say **“ESP32 Pico”** usually mean this same MCU — not a different ESP32 variant. Portal and memory limits apply equally whether you use the Atomic RS485 Base or another transceiver.

**Three stacks, two pin maps:**

| Path | RS485 module | How it attaches | PlatformIO env | UART pins |
|------|--------------|-----------------|----------------|-----------|
| **Recommended (tub-side)** | [Atomic RS485 Base](https://docs.m5stack.com/en/atom/Atomic%20RS485%20Base) | Tail stack | `M5AtomLite-tub` / `-ota` | **RX 22 / TX 19** (env default — omit pin overrides in `config.h`) |
| **Alternate** | [Tail485](https://docs.m5stack.com/en/atom/tail485) | **Tail stack** (like Atomic base — **not** Grove) | `M5AtomLite-tub` / `-ota` + **`#undef` / 32/26 in `config.h`** | **RX 32 / TX 26** per [M5 pin map](https://docs.m5stack.com/en/atom/tail485) |
| **Alternate** | [Unit RS485](https://docs.m5stack.com/en/unit/rs485) | **Grove** cable | `M5AtomLite-tub` / `-ota` + **`#undef` / 32/26 in `config.h`** | **RX 32 / TX 26** (same pins, different connector) |

Tail485 and Unit RS485 share the **same Atom Lite UART pins** (**G32 = RX**, **G26 = TX**) but attach differently. On Atom Lite hardware, keep **`M5AtomLite-tub`** (correct `m5stack-atom` board, USB upload, status LED, pin guard) and **uncomment the `#undef` / 32/26 block** in [`config-example.h`](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/src/config-example.h). **`#ifndef`-only** pin lines do **not** override the env; **`#undef` then `#define`** does.

**Generic ESP32 dev board (`esp32dev`):** same tub-side firmware as **`ESP32ota`**, but first USB flash uses **`ESP32usb`** (`upload_protocol = esptool`). **`pio run` has no `--upload-protocol` flag** — pick the env instead. After the device is on Wi‑Fi, use **`ESP32ota`** for espota updates. Do not confuse **`ESP32serial`** — that builds **`REMOTE_CLIENT`** (kitchen TCP client), not tub RS485.

**Common mistakes**

1. **Wrong pins on `M5AtomLite-tub`** — env defaults **22/19** for Atomic base. Tail485 / Unit RS485 need **32/26** — enable the **`#undef` block** in `config.h`. Leaving env defaults while wired to 32/26 talks to the wrong GPIOs.
2. **Unsafe pins** — default **GPIO 16/17** in `config-example.h` are PICO flash on Atom Lite. **Do not use them.** Immediate safe mode on 16/17 applies on **`M5AtomLite-tub`** (defines `M5_STATUS_LED_PIN=27`). On **`ESP32ota`** with 16/17, the pin guard is **not** compiled in and flash-pin UART may WDT/panic first.
3. **Manual DE/RE from the Atom is not supported** — there is no `RS485_DIR_PIN` define, and **`AUTO_TX false` toggles the UART TX data pin**, not a separate direction GPIO. Use **`AUTO_TX true`** and treat the link as plain UART: **Atomic RS485 Base** and **Unit RS485** are auto-direction; **Tail485** exposes only TX/RX to the Atom (DE/RE handled on the Tail485 board — see M5 docs). If your module needs a **separate DE/RE line driven from an ESP32 GPIO**, that is **unsupported** unless firmware adds a direction-pin option.

Community report (unverified on **v2.28+**): [Issue #31](https://github.com/shomanjk/esp32_balboa_spa/issues/31) — Atom Lite + **Tail485**; RS485 worked initially; web portal hung on firmware before **2.28** portal fixes. See [Hardware field notes](Hardware-field-notes).

---

## Bring-up checklist (copy for new boards)

Use this when adding or validating hardware. Paste into an Issue or a field-notes row when done.

```text
Board / SKU:
Role (tub-side RS485 / remote client / …):
PlatformIO env name (existing or proposed):
platformio.ini — board =
platformio.ini — extra build_flags (USB CDC, chip define, pin -D, …):
Partitions: spa_module.csv OK? (resize for larger flash?)
src/config.h — Wi-Fi / MQTT (pins only if generic env):
UART port (default Serial2 in this project):
Optional DE/RE or transceiver wiring notes:
Mechanical stack (e.g. M5 Atom on Atomic RS485 Base): verified compatible? doc link?
Optional RGB / status LED: **`M5_STATUS_LED`** + **`M5_STATUS_LED_PIN`** (FastLED)?
Libraries added to env vs [com]: lib_deps = …
USB upload / bootloader / CDC quirks:
Smoke test: serial log, /status, RS485 counters (/api/rs485):
Firmware version (/api/version):
```

**Tub-side compile-time feature flags** used by shipped gateway envs mirror [`platformio.ini`](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/platformio.ini): typically **`LOCAL_CLIENT`**, **`LOCAL_CONNECT`**, and **`BRIDGE`** (plus shared **`PRODUCTION`** / logging). The RS485 framing and MQTT stack do not depend on which ESP32 variant you pick; **pins, bootloader, partitions, and M5 libs** do.

---

## Bring-up: M5 AtomS3 Lite

This target is the **[AtomS3 Lite](https://docs.m5stack.com/en/core/AtomS3%20Lite)** (SKU **C124**, no LCD) — **not** the [AtomS3](https://docs.m5stack.com/en/core/AtomS3) / AtomS3R display modules.

Official hardware docs:

- [M5 AtomS3 Lite](https://docs.m5stack.com/en/core/AtomS3%20Lite) (ESP32-S3FN8, 8 MB flash)
- [Atomic RS485 Base](https://docs.m5stack.com/en/atom/Atomic%20RS485%20Base) (lists AtomS3-Lite as compatible; silk **RX:22 / TX:19** is **Atom Lite** naming only)
- Compare: [M5 Atom Lite](https://docs.m5stack.com/en/core/ATOM%20Lite) (**supported** tub-side stack)

### Env and pins

- PlatformIO: **`M5AtomS3Lite-tub`** (USB CDC) and **`M5AtomS3Lite-tub-ota`** (espota) — `board = esp32-s3-devkitc-1`, same tub-side flags as Atom Lite (`LOCAL_CLIENT` + `LOCAL_CONNECT` + `BRIDGE` + `DIAG_FAULT_CAPTURE`).
- UART on Atomic base with AtomS3 Lite: **`TX485_Rx=5`**, **`TX485_Tx=6`**, **`AUTO_TX`** (set by the env).
- **RGB LED:** **`M5_STATUS_LED`** + **`M5_STATUS_LED_PIN=35`** drives the onboard WS2812C via FastLED (same path as Atom Lite pin **27**). Meanings: green = Wi‑Fi up, red = down, blue/yellow = coarse RS485 activity.
- Fill `[env:M5AtomS3Lite-tub]` USB ports and/or `[env:M5AtomS3Lite-tub-ota]` `upload_port` in `platformio_local.ini` (see `platformio_local.ini.example`).
- **USB flash quirk:** if esptool connects then fails with **`No serial data received`** after the stub, enter **download mode** (hold **reset ~2 s** until the internal green LED, release) and immediately re-run `pio run -e M5AtomS3Lite-tub -t upload`. Env uses **`upload_speed = 115200`** for more reliable CDC. Close any serial monitor first. After the first successful USB flash, prefer **`pio run -e M5AtomS3Lite-tub-ota -t upload`** for updates.

### Desk bench without disturbing a spa Atom Lite

1. Leave the installed Atom Lite powered and do **not** run `M5AtomLite-tub` / `-ota` upload while testing.
2. One private `src/config.h` for Wi‑Fi / MQTT. For Docker Mosquitto on a laptop, temporarily set `MQTT_SERVER` to the Mac **LAN IP** (not `127.0.0.1`), empty broker auth, and `#define MQTT_HA_DISCOVERY 0` — see [`docker/mosquitto/`](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/docker/mosquitto/docker-compose.yml). Restore production broker settings before any Atom Lite rebuild.
3. Power the AtomS3 Lite with **USB-C** only; do not parallel both gateways on the live spa RS485 bus.
4. `pio run -e M5AtomS3Lite-tub -t upload` (first flash) — confirm hostname `spa-<mac>`, `/status` loads (no spa frames until RS485 is connected later). Later updates: `pio run -e M5AtomS3Lite-tub-ota -t upload`.

### Still deferred

- Optional [`spa_module.csv`](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/spa_module.csv) resize for 8 MB (current 4 MB-oriented table is fine; unused flash at the end)
- CI matrix entry for `M5AtomS3Lite-tub` (after local USB compile/smoke)

---

## Wiki publishing

Repo copies under **`wiki/`** sync to GitHub Wiki via CI (see [`wiki/BOOTSTRAP.md`](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/wiki/BOOTSTRAP.md) and `.github/workflows/publish-wiki.yml`).

---

 [← Wiki home](Home) · [Getting started](Getting-started) · [Hardware field notes](Hardware-field-notes)
