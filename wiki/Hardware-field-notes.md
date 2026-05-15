# Hardware field notes

Community-reported combinations of **spa panel**, **board**, and **PlatformIO environment**. This is not a compatibility guarantee — add what you tested.

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

## Reported setups

| Tub / panel (report) | Board / stack | PlatformIO env | RS485 notes | Outcome |
|----------------------|---------------|----------------|-------------|---------|
| Balboa **BP501**; agency **`BP501-CL501X1-AS`**; also stamped **CL501X1** (heater/pack segment — see above) | *(reporter: add your gateway — e.g. M5 Atom Lite + Atomic RS485)* | `M5AtomLite-tub` (typical) | Standard Balboa RS485; [physical layer](https://github.com/ccutrer/balboa_worldwide_app/wiki#physical-layer); BWG [BP 501](https://www.balboawatergroup.com/BP501/) | Field note — **reporter** (confirm gateway + firmware `/api/version`) |
| *(example)* Balboa BP7xxx | M5 Atom Lite + Atomic RS485 Base | `M5AtomLite-tub` | A/B as labeled on base; `AUTO_TX true` | Example row |

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
