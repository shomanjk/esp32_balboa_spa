# Spa controllers and brands

Catalog of **Balboa control packs** and **OEM tub brands** for discoverability. This page lists what might work and what still needs a field report — it is **not** a compatibility guarantee.

**Tested setups only:** see [Hardware field notes](Hardware-field-notes) for community-verified combinations (board, env, wiring, outcome).

**To add proof:** add a row on [Hardware field notes](Hardware-field-notes) or open an [Issue](https://github.com/shomanjk/esp32_balboa_spa/issues) with the report template there.

---

## Identify your controller

Tub brand alone does not determine compatibility — many OEMs ship different packs by year and model. Confirm the **Balboa pack label** on the control box inside the spa cabinet.

| What you see | Typical meaning |
|----------------|------------------|
| **BP501** | **Balboa control pack family** — Balboa Water Group documents **BP501** (North America) and **BP601** (Europe) as the “BP 501/601” control system line. See [BP 501/601 — Balboa Water Group](https://www.balboawatergroup.com/BP501/). |
| **CL501X1** | Often printed as the **heater / spa-pack configuration** segment bundled with BP501. It is **not** a competing protocol name — tub-side RS485 is still Balboa **115200 8N1** ([protocol.md — RS-485](https://github.com/ccutrer/balboa_worldwide_app/blob/main/doc/protocol.md)). |
| **Agency Model `BP501-CL501X1-AS`** | **Full agency / certification-style designation**: pack family + configuration segment + regional suffix. Prefer this **full string** in field reports. |

Also check:

- **Pack label** — white or silver sticker on the control box (five-digit Balboa part numbers are common).
- **Runtime system model** — after the gateway connects, **`/config`** or the information response **system model** field (e.g. `MQBP501`, `BP2000G1` in [protocol.md examples](https://github.com/ccutrer/balboa_worldwide_app/blob/main/doc/protocol.md)).

**Out of scope for this project:** pre-BP **GS/VS** Balboa packs and non-Balboa controllers (Gecko, HydroQuip, SpaGuts, etc.). This firmware targets Balboa **115200 8N1** RS485 only. Generic bus pinout: [ccutrer physical layer](https://github.com/ccutrer/balboa_worldwide_app/wiki#physical-layer).

---

## Gateway status labels

Every catalog row below uses one of these statuses:

| Status | Meaning |
|--------|---------|
| **Verified** | At least one field report in [Hardware field notes](Hardware-field-notes) |
| **Likely** | Same protocol family; upstream or BWA evidence, but no `esp32_balboa_spa` tub-side report yet |
| **Unknown** | Publicly documented Balboa family; needs first report |
| **Out of scope** | Different protocol / not Balboa RS485 |

**Do not upgrade status without evidence:** Unknown → Likely needs a protocol-family citation; Likely or Unknown → Verified needs a field report.

---

## Balboa controller catalog

| Controller family | Region / notes | Gateway status | RS485 / harness notes | Field reports |
|-------------------|----------------|----------------|----------------------|---------------|
| **BP501** / **BP601** (501/601 line) | BP501 NA; BP601 EU/international; runtime models e.g. `MQBP501`, `MBP501UX` | **Verified** | [Physical layer](https://github.com/ccutrer/balboa_worldwide_app/wiki#physical-layer); BP501 maintainer report: [Molex `0451320403`](https://www.digikey.com/short/p5ctrr0m) on factory header — see [Hardware field notes · Parts](Hardware-field-notes#parts-that-worked-community) | [Hardware field notes · Reported setups](Hardware-field-notes#reported-setups) |
| **BP2100** (incl. G0 variants) | Mid-tier line; upstream esp8266 era | **Likely** | [Physical layer](https://github.com/ccutrer/balboa_worldwide_app/wiki#physical-layer) | — |
| **BP2000** (incl. G1) | BWA example `System Model: BP2000G1` | **Likely** | [Physical layer](https://github.com/ccutrer/balboa_worldwide_app/wiki#physical-layer) | — |
| **BP7000** / **BP7** series | Common modern BP line | **Unknown** | [Physical layer](https://github.com/ccutrer/balboa_worldwide_app/wiki#physical-layer) | — |
| **BP600** | Related to 601 line; NA/EU variants | **Unknown** | [Physical layer](https://github.com/ccutrer/balboa_worldwide_app/wiki#physical-layer) | — |
| **BP100** / **BP200** | Older everyday line per BWG product history | **Unknown** | [Physical layer](https://github.com/ccutrer/balboa_worldwide_app/wiki#physical-layer) | — |
| **BP300** / **BP400** | Intermediate generations | **Unknown** | [Physical layer](https://github.com/ccutrer/balboa_worldwide_app/wiki#physical-layer) | — |
| **BP500** (legacy, pre-501) | Distinct from BP501 naming | **Unknown** | [Physical layer](https://github.com/ccutrer/balboa_worldwide_app/wiki#physical-layer) | — |
| **BP900** / **BP1000** | Older packs; still seen in retrofit listings | **Unknown** | [Physical layer](https://github.com/ccutrer/balboa_worldwide_app/wiki#physical-layer) | — |
| **BP1500** / **BP1600** | Mid-tier historical lines | **Unknown** | [Physical layer](https://github.com/ccutrer/balboa_worldwide_app/wiki#physical-layer) | — |
| **BP2500** | Larger residential / swim-spa adjacent | **Unknown** | [Physical layer](https://github.com/ccutrer/balboa_worldwide_app/wiki#physical-layer) | — |
| **BP3000** | Higher-capacity line | **Unknown** | [Physical layer](https://github.com/ccutrer/balboa_worldwide_app/wiki#physical-layer) | — |
| **EL** / **GL** series (Balboa-labeled) | Older Balboa-branded packs — verify chip sticker | **Unknown** | [Physical layer](https://github.com/ccutrer/balboa_worldwide_app/wiki#physical-layer) | — |
| **GS** / **VS** series | Pre-BP era; BP topsides do not imply GS compatibility | **Out of scope** | Not Balboa BP RS485 protocol | — |
| **Wi-Fi module slot (0350-era)** | Accessory on some BP packs; not a pack family | **Unknown** | May already occupy RS485 channel **0x0A** — verify before adding a second client | — |

Add pack-specific connector or harness notes here only when sourced from a field report.

---

## OEM brand index

Many hot tub brands use Balboa control packs in **some** models. **Verify the pack label** before assuming compatibility.

| Brand | Example tub line | Typical controller | What to photograph | Reports |
|-------|------------------|--------------------|--------------------|---------|
| **Divine Spas** | **Sinclair** | Maintainer Sinclair: **Balboa BP501** (`BP501-CL501X1-AS`) — other Divine models may differ | Pack label, agency model, topside part number | [Hardware field notes · BP501 row](Hardware-field-notes#reported-setups) |
| **Jacuzzi** | — | Often Balboa BP series in many models — **verify label** | Pack label, agency model, topside part number | — |
| **Hot Spring** (Watkins) | — | Often Balboa BP series in many models — **verify label** | Pack label, agency model, topside part number | — |
| **Sundance** | — | Often Balboa BP series in many models — **verify label** | Pack label, agency model, topside part number | — |
| **Cal Spas** | — | Often Balboa BP series in many models — **verify label** | Pack label, agency model, topside part number | — |
| **Master Spas** | — | Often Balboa BP series in many models — **verify label** | Pack label, agency model, topside part number | — |
| **Coast Spas** | — | Often Balboa BP series in many models — **verify label** | Pack label, agency model, topside part number | — |
| **Marquis** | — | Often Balboa BP series in many models — **verify label** | Pack label, agency model, topside part number | — |
| **Artesian** | — | Often Balboa BP series in many models — **verify label** | Pack label, agency model, topside part number | — |
| **Coleman** | — | Often Balboa BP series in many models — **verify label** | Pack label, agency model, topside part number | — |
| **Viking** | — | Often Balboa BP series in many models — **verify label** | Pack label, agency model, topside part number | — |
| **Dynasty** | — | Often Balboa BP series in many models — **verify label** | Pack label, agency model, topside part number | — |
| **Beachcomber** | — | Often Balboa BP series in many models — **verify label** | Pack label, agency model, topside part number | — |
| **Arctic Spas** | — | Often Balboa BP series in many models — **verify label** | Pack label, agency model, topside part number | — |
| **LA Spas** | — | Often Balboa BP series in many models — **verify label** | Pack label, agency model, topside part number | — |
| **Dimension One** | — | Often Balboa BP series in many models — **verify label** | Pack label, agency model, topside part number | — |

**Tub model naming:** do not add arbitrary tub model numbers here unless there is a verified field report. **Sinclair** is listed because it maps to the maintainer’s verified BP501 setup.

---

## Contributing

| Change type | Where |
|-------------|-------|
| Update catalog status, RS485 notes, or brand stubs | Edit this page |
| Add proof a setup works (tub + board + wiring) | [Hardware field notes](Hardware-field-notes) |
| Firmware APIs, MQTT topics, release behavior | Main repo [README](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/README.md) / [CHANGELOG](https://github.com/shomanjk/esp32_balboa_spa/blob/ESP32/CHANGELOG.md) in a PR; link from wiki |

Protocol bytes and message decoding: [ccutrer/balboa_worldwide_app wiki](https://github.com/ccutrer/balboa_worldwide_app/wiki) and [protocol.md](https://github.com/ccutrer/balboa_worldwide_app/blob/main/doc/protocol.md) — link from here; do not duplicate long tables.

---

[← Wiki home](Home) · [Hardware field notes](Hardware-field-notes) · [Getting started](Getting-started)
