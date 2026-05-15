# esp32_balboa_spa Wiki

ESP32 firmware that talks to a **Balboa** spa controller over **RS485**: read status, control equipment from the web portal, and publish state over **MQTT** / **Home Assistant**.

**Last reviewed for firmware:** 2.9.0 (see [`VERSION`](https://github.com/shomanjk/esp32_balboa_spa/blob/main/src/main.h) in the main repo).

---

## Canonical docs (versioned with releases)

These live in the [main repository](https://github.com/shomanjk/esp32_balboa_spa) and should be treated as the source of truth for build flags, APIs, and release behavior:

- [README](https://github.com/shomanjk/esp32_balboa_spa/blob/main/README.md) — features, build, M5 wiring, MQTT/HA topics, OTA, compiler flags
- [CHANGELOG](https://github.com/shomanjk/esp32_balboa_spa/blob/main/CHANGELOG.md) — release history
- [GitHub Releases](https://github.com/shomanjk/esp32_balboa_spa/releases) — tagged snapshots
- [OTA + live logging workflow](https://github.com/shomanjk/esp32_balboa_spa/blob/main/OTA_LOGGING_WORKFLOW.md)
- [FORK.md](https://github.com/shomanjk/esp32_balboa_spa/blob/main/FORK.md) — fork maintenance and tags

---

## Wiki guides

| Page | Purpose |
|------|---------|
| [Getting started](Getting-started) | First-time clone, `config.h`, flash, verify `/status` |
| [Hardware targets](Hardware-targets) | Supported vs planned boards, bring-up checklist (e.g. AtomS3) |
| [Troubleshooting FAQ](Troubleshooting-FAQ) | Symptom → checks → deeper repo links |
| [Home Assistant setup](Home-Assistant-setup) | Broker, discovery, temp units, example automation |
| [Hardware field notes](Hardware-field-notes) | Community-tested tubs and boards |

---

## External references (protocol & wiring)

- [ccutrer/balboa_worldwide_app wiki](https://github.com/ccutrer/balboa_worldwide_app/wiki) — especially [physical layer / finding wires](https://github.com/ccutrer/balboa_worldwide_app/wiki#physical-layer)
- [protocol.md](https://github.com/ccutrer/balboa_worldwide_app/blob/main/doc/protocol.md) — frame formats and command semantics

---

## Contributing to this wiki

- **Field reports:** Add a row on [Hardware field notes](Hardware-field-notes) (tub model, board, PlatformIO env, outcome).
- **Corrections:** Edit the wiki page directly, or open an [Issue](https://github.com/shomanjk/esp32_balboa_spa/issues) with tub model + board + what you tried.
- **Behavior that depends on a firmware release** (new MQTT topics, flags, APIs): update the **main repo README/CHANGELOG** in a PR; link from the wiki, do not duplicate long tables here.
