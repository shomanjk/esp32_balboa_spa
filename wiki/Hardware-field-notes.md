# Hardware field notes

Community-reported combinations of **spa panel**, **board**, and **PlatformIO environment**. This is not a compatibility guarantee — add what you tested.

For **planned** boards (no checked-in env yet), proposed bring-up steps and checklists live on **[Hardware targets](Hardware-targets)**.

**To add a row:** edit this page or open an [Issue](https://github.com/shomanjk/esp32_balboa_spa/issues) with the same fields.

---

## Reported setups

| Tub / panel (report) | Board / stack | PlatformIO env | RS485 notes | Outcome |
|----------------------|---------------|----------------|-------------|---------|
| *(example)* Balboa BP7xxx | M5 Atom Lite + Atomic RS485 Base | `M5AtomLite-tub` | A/B as labeled on base; `AUTO_TX true` | Works |
| *(add yours)* | | | | |

---

## Suggested fields for new reports

Copy this template into the table or an Issue:

```text
Tub / panel model:
Board (e.g. M5 Atom + Atomic RS485, generic ESP32):
PlatformIO env:
TX485_Rx / TX485_Tx:
AUTO_TX (true/false):
A/B wiring notes:
Firmware version (/api/version):
What works:
What does not:
```

---

## Official hardware docs in the main repo

- [Hardware targets — bring-up checklist](Hardware-targets) — planned boards (e.g. AtomS3) before they have a PlatformIO env
- [M5 Atom Lite + Atomic RS485 Base](https://github.com/shomanjk/esp32_balboa_spa/blob/main/README.md#m5-atom-lite--atomic-rs485-base-tub-side)
- [config-example.h](https://github.com/shomanjk/esp32_balboa_spa/blob/main/src/config-example.h) — pins for M5 stack, generic ESP32, Unit RS485

Bus tapping and wire colors: [ccutrer BWA wiki — physical layer](https://github.com/ccutrer/balboa_worldwide_app/wiki#physical-layer).

---

[← Wiki home](Home)
