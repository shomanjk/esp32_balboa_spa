# esp32_balboa_spa Wiki

ESP32 firmware that talks to a **Balboa** spa controller over **RS485**: read status, control equipment from the web portal, and publish state over **MQTT** / **Home Assistant**.

**Full overview (value, parts, setup, features):** [README · Overview](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/README.md#overview) in the main repo.

**Last reviewed for firmware:** 2.10.0 (see [`VERSION`](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/src/main.h) in the main repo).

---

## Canonical docs (versioned with releases)

These live in the [main repository](https://github.com/shomanjk/esp32_balboa_spa) and should be treated as the source of truth for build flags, APIs, and release behavior. File links below use the **`ESP32`** branch (GitHub default for this fork); **`main`** may lag and return **404** for some paths.

- [README](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/README.md) — [overview](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/README.md#overview), build, M5 wiring, MQTT/HA topics, OTA, compiler flags
- [CHANGELOG](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/CHANGELOG.md) — release history
- [GitHub Releases](https://github.com/shomanjk/esp32_balboa_spa/releases) — tagged snapshots
- [OTA + live logging workflow](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/OTA_LOGGING_WORKFLOW.md)
- [FORK.md](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/FORK.md) — fork maintenance and tags

---

## Wiki guides

| Page | Purpose |
|------|---------|
| [Getting started](Getting-started) | First-time clone, `config.h`, flash, verify `/status` |
| [Hardware targets](Hardware-targets) | Supported vs planned boards, bring-up checklist (e.g. AtomS3) |
| [Troubleshooting FAQ](Troubleshooting-FAQ) | Symptom → checks → deeper repo links |
| [Home Assistant setup](Home-Assistant-setup) | Broker, discovery, temp units, example automation |
| [Spa controllers and brands](Spa-controllers-and-brands) | Balboa pack catalog, OEM brand index, compatibility status |
| [Hardware field notes](Hardware-field-notes) | Community-tested tubs and boards |

---

## External references (protocol & wiring)

- [ccutrer/balboa_worldwide_app wiki](https://github.com/ccutrer/balboa_worldwide_app/wiki) — especially [physical layer / finding wires](https://github.com/ccutrer/balboa_worldwide_app/wiki#physical-layer)
- [protocol.md](https://github.com/ccutrer/balboa_worldwide_app/blob/main/doc/protocol.md) — frame formats and command semantics

---

## Contributing to this wiki

- **Catalog stubs:** Edit status, RS485 notes, and OEM brands on [Spa controllers and brands](Spa-controllers-and-brands).
- **Field reports:** Add a row on [Hardware field notes](Hardware-field-notes) (tub model, board, PlatformIO env, outcome).
- **Corrections:** Edit the wiki page directly, or open an [Issue](https://github.com/shomanjk/esp32_balboa_spa/issues) with tub model + board + what you tried.
- **Behavior that depends on a firmware release** (new MQTT topics, flags, APIs): update the **main repo README/CHANGELOG** in a PR; link from the wiki, do not duplicate long tables here.
