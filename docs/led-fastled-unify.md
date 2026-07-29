# Status LED: unify on FastLED (deferred)

**Status:** parked — do not implement until Atom Lite LED colors can be checked on hardware.  
**Why parked:** AtomS3 Lite already uses FastLED (GPIO **35**). Atom Lite still uses **`M5Atom`** `M5.dis.drawpix` under **`M5_ATOM_LED`**. Unifying avoids dual backends but changes the spa Atom Lite LED stack and needs a color smoke test before shipping.

## Goal

One status-LED path for both M5 tub envs:

| Board | Env today | LED today | Target |
|-------|-----------|-----------|--------|
| Atom Lite | `M5AtomLite-tub` | `M5_ATOM_LED` + M5Atom | FastLED, pin **27** |
| AtomS3 Lite | `M5AtomS3Lite-tub` | `M5_ATOMS3_LITE_LED` + FastLED pin **35** | Same FastLED code, pin from env |

Suggested flags (names flexible):

```ini
'-DM5_STATUS_LED'
'-DM5_STATUS_LED_PIN=27'   ; Atom Lite
'-DM5_STATUS_LED_PIN=35'   ; AtomS3 Lite
```

Drop **`m5stack/M5Atom`** from Atom Lite `lib_deps` if nothing else needs it (LED-only today).

## Checklist (when ready)

1. Bench or spare **Atom Lite** (not required to touch the live spa unit first): flash a FastLED-on-27 build; confirm **green** = Wi‑Fi up, **red** = down, **blue/yellow** RS485 flashes match today’s meaning.
2. Collapse [`src/led_control.cpp`](../src/led_control.cpp) / [`.h`](../src/led_control.h) to a single FastLED backend; update [`src/main.ino`](../src/main.ino) and [`src/rs485_led_hooks.cpp`](../src/rs485_led_hooks.cpp) to one define.
3. Update [`platformio.ini`](../platformio.ini) Atom Lite / AtomS3 Lite envs; remove obsolete `M5_ATOM_LED` / `M5_ATOMS3_LITE_LED` (or keep as aliases for one release if needed).
4. Docs: README compiler flags, AGENTS hardware table, wiki Hardware-targets.
5. Compile `M5AtomLite-tub` + `M5AtomS3Lite-tub`; field-check AtomS3 Lite still green on Wi‑Fi.
6. Only then OTA/flash the **spa** Atom Lite when intentionally updating that device.

## Out of scope here

- AtomS3 (display) / AtomS3R LED APIs  
- Growing flash partitions for 8 MB  
- `M5AtomS3Lite-tub-ota`

## Related

- Current split: [`src/led_control.cpp`](../src/led_control.cpp)  
- Bring-up notes: [wiki Hardware-targets](../wiki/Hardware-targets.md)
