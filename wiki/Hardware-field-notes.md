# Hardware field notes

Community-reported combinations of **spa panel**, **board**, and **PlatformIO environment**. This is not a compatibility guarantee — add what you tested.

For the full controller and OEM brand index, see **[Spa controllers and brands](Spa-controllers-and-brands)**. **This page is for tested setups only.**

For **planned** boards (no checked-in env yet), proposed bring-up steps and checklists live on **[Hardware targets](Hardware-targets)**.

**To add a row:** edit this page or open an [Issue](https://github.com/shomanjk/esp32_balboa_spa/issues) with the same fields.

---

## Reading BPxxx, CLxxx, and “Agency Model” strings

These labels come from **different layers** of the same system, so it is normal to see more than one “model” on a sticker:

| What you see | Typical meaning |
|----------------|------------------|
| **BP501** | **Balboa control pack family** — Balboa Water Group documents **BP501** (North America) and **BP601** (Europe) as the “BP 501/601” control system line. See [BP 501/601 — Balboa Water Group](https://www.balboawatergroup.com/BP501/). |
| **CL501X1** | Often printed as the **heater / spa-pack configuration** segment that dealers bundle with BP501 (parts listings frequently say “BP501 … with Heater CL501X1”). It is **not** a competing protocol name — tub-side RS485 for these packs is still Balboa’s usual **115200 8N1** framing (see [protocol.md — RS-485](https://github.com/ccutrer/balboa_worldwide_app/blob/master/doc/protocol.md)). |
| **Agency Model `BP501-CL501X1-AS`** | **Full agency / certification-style designation**: pack family (**BP501**) + configuration segment (**CL501X1**) + suffix (**AS**, often an agency or regional variant). Prefer this **full string** in the table below for searchability; keep **CL501X1** alongside it if the short code is what you remember. |

The firmware’s **information** response can expose a **system model** ASCII field (e.g. variants like `MQBP501 ` in examples from [protocol.md — Information Response](https://github.com/ccutrer/balboa_worldwide_app/blob/master/doc/protocol.md)); that is the runtime name from the controller, not the same typography as the enclosure label.

---

## Parts that worked (community)

Not a compatibility guarantee — verify pinout, wire gauge, and crimp tooling for your tub. You still need the correct **terminals** crimped into the housing (Molex publishes mate part numbers separately).

| Part | Supplier | Use |
|------|----------|-----|
| **Molex `0451320403`** (4-circuit plug housing) | [DigiKey `WM16117-ND`](https://www.digikey.com/short/p5ctrr0m) | On a **Balboa BP501** tub, maintainer report: mates cleanly with the **factory-installed header** on the spa board so you can run a short harness to the M5 Atomic RS485 base **VH‑3.96** / bus terminals without cutting the factory harness. Confirm **12 V / GND / A / B** (or your board’s pin order) against [ccutrer physical layer](https://github.com/ccutrer/balboa_worldwide_app/wiki#physical-layer) and your label before energizing. |

Add other SKUs here when you have a tested combination.

---

## Reported setups

| Tub / panel (report) | Board / stack | PlatformIO env | RS485 notes | Outcome |
|----------------------|---------------|----------------|-------------|---------|
| **Divine Spas Sinclair**; Balboa **BP501**; agency **`BP501-CL501X1-AS`**; also stamped **CL501X1** (heater/pack segment — see above) | [M5 Atom Lite](https://docs.m5stack.com/en/core/ATOM%20Lite) + [Atomic RS485 Base](https://docs.m5stack.com/en/atom/Atomic%20RS485%20Base) (stacked tub-side gateway) | `M5AtomLite-tub` | **`TX485_Rx` 22**, **`TX485_Tx` 19**; **`AUTO_TX true`**; A/B per base labeling (swap if no frames). **Harness:** [Molex `0451320403`](https://www.digikey.com/short/p5ctrr0m) (DigiKey `WM16117-ND`) plugged onto the **factory header** on the spa board → short leads to Atomic base (see [Parts that worked](#parts-that-worked-community)). [physical layer](https://github.com/ccutrer/balboa_worldwide_app/wiki#physical-layer); BWG [BP 501](https://www.balboawatergroup.com/BP501/) | **Works** — maintainer tub-side stack ([README](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/README.md#m5-atom-lite--atomic-rs485-base-tub-side), [`config-example.h`](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/src/config-example.h)) |

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
- [M5 Atom Lite + Atomic RS485 Base](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/README.md#m5-atom-lite--atomic-rs485-base-tub-side)
- [config-example.h](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/src/config-example.h) — pins for M5 stack, generic ESP32, Unit RS485

Bus tapping and wire colors: [ccutrer BWA wiki — physical layer](https://github.com/ccutrer/balboa_worldwide_app/wiki#physical-layer).

---

[← Wiki home](Home)
