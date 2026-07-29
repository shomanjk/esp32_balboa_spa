# Status LED: unify on FastLED

**Status:** implemented and **desk Atom Lite color-verified** (red → green on Wi‑Fi; same as prior M5Atom path).  
**Goal met:** one FastLED backend for Atom Lite (GPIO **27**) and AtomS3 Lite (GPIO **35**); `m5stack/M5Atom` removed from tub envs.

## Flags

```ini
'-DM5_STATUS_LED'
'-DM5_STATUS_LED_PIN=27'   ; Atom Lite
'-DM5_STATUS_LED_PIN=35'   ; AtomS3 Lite
```

Obsolete: **`M5_ATOM_LED`**, **`M5_ATOMS3_LITE_LED`**.

## Colors (unchanged meaning)

| Color | Meaning |
|-------|---------|
| Green | Wi‑Fi has IP |
| Red | Wi‑Fi disconnected |
| Blue flash | Coarse RS485 TX activity |
| Yellow flash | Coarse RS485 RX activity |

## Smoke test (desk Atom Lite)

1. ~~`pio run -e M5AtomLite-tub -t upload` (USB).~~
2. ~~Confirm **red** briefly at boot / disconnect, **green** after Wi‑Fi IP.~~ **Verified 2026-07-29** (desk unit).
3. With RS485 traffic (or spa connected): brief **blue/yellow** flashes, then back to green/red.
4. Optional: flash AtomS3 Lite and confirm green still correct (same FastLED code path as before unify).

## Related

- [`src/led_control.cpp`](../src/led_control.cpp)
- [wiki Hardware-targets](../wiki/Hardware-targets.md)
