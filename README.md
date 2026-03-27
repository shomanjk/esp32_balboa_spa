# esp32_balboa_spa

## About this fork

This repo — **[shomanjk/esp32_balboa_spa](https://github.com/shomanjk/esp32_balboa_spa)** on GitHub — is where **ongoing maintenance and changes** are tracked. The older ESP8266 / related projects this code descends from (see [Background / History](#background--history)) are **not under active development**; we are **not** planning to open pull requests there.

- **What changed:** [CHANGELOG.md](CHANGELOG.md) and [GitHub Releases](https://github.com/shomanjk/esp32_balboa_spa/releases) (when you publish them).
- **Fork workflow (push vs PR vs tags):** [FORK.md](FORK.md).
- **AI / contributor orientation:** [AGENTS.md](AGENTS.md) (architecture, flags, known gaps, `config.h`).

WiFI Enable your Balboa SPA using a ESP32 module connected to your spa controller using rs485 interface to Balboa SPA Controller.

Multimode code base, with multiple user interfaces available.  Interfaces include MQTT, Web and ePaper display.

Code base can also operate in client mode to a remote implementation of the code base in the Hot Tub.  This allows for creating a ePaper display in a central location.

In my setup I have the code base deployed twice, one connected to the Balboa spa controller via rs485.  And a second deployment on a [LilyGo T5 ePaper Display](https://www.lilygo.cc/en-ca/products/t5-4-7-inch-e-paper-v2-3?srsltid=AfmBOopva5B_jxFAsa86Fn75lR66ZpcsqNLJEqPG4Axu8zeuCEEeqI0D) that is mounted to the kitchen so you can see the temperature etc.

## Advanced Features

* Caching of hot tub configuration, to reduce number of calls to SPA Controller and improve responsiveness of client applications
* Hourly tracking of hot tub temperature for 24 hours
* Daily tracking of heater on time in seconds. Keeps 24 days of history
* Daily Tracking of filter on time in seconds.  Keeps 24 days of history

## This is the ePaper display

![alt text](docs/ePaper-Sept2024.jpeg)

Display is based on the [LilyGo T5 ePaper Display](https://www.lilygo.cc/en-ca/products/t5-4-7-inch-e-paper-v2-3?srsltid=AfmBOopva5B_jxFAsa86Fn75lR66ZpcsqNLJEqPG4Axu8zeuCEEeqI0D)

Pls note I have stopped development of ePaper functionality, as my device [stopped working.](https://github.com/Xinyuan-LilyGO/LilyGo-EPD47/issues/137)

## This is the web site
Credit for the code goes to https://github.com/jozefnad/balboa-spa

![alt text](docs/balboa-spa-web.png)

Currently the WebSite buttons are not working.  I never got around to wiring them up.

Legacy firmware-served pages (`/status`, `/config`, `/state`) now include responsive/mobile-friendly layout behavior (viewport scaling, wrapping nav, fluid chart/image sizing) for better phone usability.

## Integration with Homebridge

I have used this with the homebridge plugin [homebridge-plugin-bwaspa](https://github.com/vincedarley/homebridge-plugin-bwaspa) to control and automate my Hot Tub.

## MQTT Interface

MQTT Commands are not working yet.  I never got around to wiring them up.

# Code Base Build

For the build I use platformio.

## M5 Atom Lite + Atomic RS485 Base (tub-side)

This maintained fork targets **tub-side** builds on the [M5 Atom Lite](https://docs.m5stack.com/en/core/ATOM%20Lite) stacked on the [Atomic RS485 Base](https://docs.m5stack.com/en/atom/Atomic%20RS485%20Base) (TTL ↔ RS‑485, **SP3485EE**, built-in **12 V→5 V** DC‑DC for the Atom). Generic ESP32 dev boards remain supported; see [`src/config-example.h`](src/config-example.h) for default GPIO **16/17**.

- **PlatformIO environment:** `M5AtomLite-tub` in [`platformio.ini`](platformio.ini) uses `board = m5stack-atom` and the same partition table as other 4MB builds (`spa_module.csv`).
- **Pins:** With the Atom Lite **stacked** on the Atomic RS485 Base, UART2 uses **RX = GPIO 22**, **TX = GPIO 19** (same as M5’s [Arduino example](https://github.com/m5stack/M5-ProductExampleCodes/blob/master/AtomBase/AtomicRS485/Arduino/AtomicRS485/AtomicRS485.ino)). In `config.h` set **`TX485_Rx 22`** and **`TX485_Tx 19`**. If you see no frames, double-check **A/B** on the spa bus (and swap A/B if needed); M5 notes optional **120 Ω** termination between A and B for long runs.
- **`AUTO_TX`:** Prefer **`AUTO_TX true`** in [`src/config.h`](src/config-example.h) so the firmware does not drive a separate DE/RE line; use **`false`** only if your transceiver needs manual direction control.
- **Power:** The base can **step 12 V down to 5 V** to power the Atom (see [M5 Atomic RS485 Base](https://docs.m5stack.com/en/atom/Atomic%20RS485%20Base)). Connect the **VH‑3.96** terminal per M5’s pinout and **your** spa controller’s wiring (accessory **12 V** and **GND** are separate from the RS‑485 **A/B** data pair—use the Balboa [physical layer](https://github.com/ccutrer/balboa_worldwide_app/wiki#physical-layer) guidance). For bench work, **USB 5 V** is fine.
- **RGB LED (Atom Lite, `M5AtomLite-tub` only):** Build flag **`M5_ATOM_LED`** (set in [`platformio.ini`](platformio.ini) for this env) drives the single addressable pixel. Colors mean:
  | Color | Meaning |
  | --- | --- |
  | **Green** (steady) | Wi‑Fi station has an IP (connected to AP). |
  | **Red** (steady) | Wi‑Fi station disconnected (no IP). |
  | **Blue** (brief flash) | Shown at the start of each RS485 loop pass (coarse “TX side” hint in code; not per-byte TX). |
  | **Yellow** (brief flash) | Shown at the end of each RS485 loop pass (coarse “RX side” hint in code). |
  Blue/yellow are **not** a reliable alternating TX/RX indicator on every loop (timing in [`src/led_control.cpp`](src/led_control.cpp)); they only show the RS485 path is being serviced, not byte‑for‑byte traffic. To disable LED logic, remove or unset **`M5_ATOM_LED`** for that environment and adjust `main.ino` if needed.

Copy [`src/config-example.h`](src/config-example.h) to `src/config.h`, set Wi‑Fi / MQTT, then enable the **M5 Atom Lite + Atomic RS485 Base** UART block in `config.h` (and comment out the default GPIO16/17 lines) before building `M5AtomLite-tub`.

**Alternate hardware:** You can still use a **generic ESP32** UART (e.g. **16/17**) or wire a [M5 Unit RS485](https://docs.m5stack.com/en/unit/rs485) (Grove) to the Atom’s **HY2.0** port with **TX485_Rx 32** / **TX485_Tx 26**—see comments in [`src/config-example.h`](src/config-example.h).

## OTA updates (M5 Atom tub-side)

- OTA runtime is already enabled (`ArduinoOTA` in [`lib/wifiModule/wifiModule.cpp`](lib/wifiModule/wifiModule.cpp)); serial logs print host/IP after Wi‑Fi connect.
- Use PlatformIO environment **`M5AtomLite-tub-ota`** for Wi‑Fi uploads (`upload_protocol = espota`).
- Set `upload_port` in [`platformio.ini`](platformio.ini) to your device hostname from logs (for example `spa-XXXXXXXXXXXX.local`) or its static IP.
- Upload command:

```
pio run -e M5AtomLite-tub-ota -t upload
```

- Trusted LAN default: OTA auth is off unless enabled in `config.h` (`ENABLE_OTA_AUTH true` + `OTA_PASSWORD`).
- Recovery path: keep USB serial flashing available via `M5AtomLite-tub` in case OTA fails.
- Firmware/build visibility in web UI:
  - `/state` now shows `Firmware Version` and `Firmware Build`.
  - `/api/version` returns JSON (`version`, `build`, `hostname`, `ip`, `restartReason`) for portal/status integrations.
- Reference runbook for OTA with live logs: [`OTA_LOGGING_WORKFLOW.md`](OTA_LOGGING_WORKFLOW.md).

## Compiler Definitions

  * LOCAL_CLIENT - Connects to a local SPA via rs485 connection
  * REMOTE_CLIENT - Connects to a remote SPA via TCP / WiFi Module interface 
  * LOCAL_CONNECT - Enable discovery of ESP32 module via the Balboa discovery protocol
  * BRIDGE - Enable local TCP Server - Can be leveraged by https://github.com/vincedarley/homebridge-plugin-bwaspa
  * TELNET_LOG - Enables serial logging via a telnet interface
  * spaEpaper - Enables ePaper display
  * M5_ATOM_LED - (Optional, **`M5AtomLite-tub` only**) M5 Atom RGB LED: Wi‑Fi status + brief RS485 activity flashes; see [M5 Atom Lite + Atomic RS485 Base](#m5-atom-lite--atomic-rs485-base-tub-side).

### In spa configuration

For the unit deployed in the spa, and connected to the Balboa spa controller via rs485 I use these compiler definitions.

```
  '-DLOCAL_CONNECT'
  '-DLOCAL_CLIENT'
  '-DBRIDGE'
  '-DTELNET_LOG'
```

### Remote module configuration

This is for the [LilyGo T5 ePaper Display](https://www.lilygo.cc/en-ca/products/t5-4-7-inch-e-paper-v2-3?srsltid=AfmBOopva5B_jxFAsa86Fn75lR66ZpcsqNLJEqPG4Axu8zeuCEEeqI0D)

```
  '-DREMOTE_CLIENT'
  '-DspaEpaper'
```

## Background / History

This is port of the package to run on an ESP32 Device, and modernization of the package

Based on the great work over at \
https://github.com/cribskip/esp8266_spa
https://github.com/ccutrer/balboa_worldwide_app/wiki
https://github.com/ccutrer/balboa_worldwide_app/blob/master/doc/protocol.md


# Original README by cribskip

# esp8266_spa
Control for a Balboa spa controller using the esp8266 (tested on BP2100 and BP601 series)

The sketch connects to the tub, gets an ID and spits out the state to the MQTT broker on topics "Spa/#".
You can control the tub using the subscribed topics, f.e. "Spa/light" with message "ON" for maximum compatability with openhab.

Maybe you need to adjust the sketch to your tub configuration (number of pumps, connection of blower, nr of lights...). You may find the DEBUG comments useful for this task.

Bonus: you may add several relays or such like I did ;-)

# Getting started
- Make sure your Spa is using a Balboa controller
- This is known to work on and developed on a BP2100G0 and BP601 series controllers. Yours may be compatible.
- Get the parts
- Flash the esp8266 fom the Arduino IDE or PlatformIO
- Connect everything together
- Hook up on the Spa
- Enjoy and get tubbin' ;-)

# Parts
- Get a esp8266, perferable a Wemos D1 Pro in case you need to attach a seperate antenna
- RS485 bus transceiver, (for example the "ARCELI TTL To RS485 Adapter 485 Serial Port UART Level Converter Module 3.3V 5V")
- A DC-DC converter for powering from the Tub (LM2596 for example)
- breadboard, wire etc...


# Hardware connections
![Example](https://github.com/EmmanuelLM/esp8266_spa/blob/master/esp8266_spa_bb.png)
- Look up finding the right wires on https://github.com/ccutrer/balboa_worldwide_app/wiki#physical-layer
- Connect the DC-DC converter to the supply wires (+ and Ground) from the Tub
- Set the DC-DC converter to output 3.3V. This output voltage (+ Ground) should then connect to the Wemos D1 Mini Pro and the RS485 transceiver
- Connect the RS485 transceiver to the A and B wires
- Connect the esp8266-TX to the RS485 TX
- Connect the esp8266-RX to the RS485 RX

![Example](https://github.com/EmmanuelLM/esp8266_spa/blob/master//PXL_20210101_104120166.jpg)

# Debug
- First, check your voltages - the system is running at 3.3V
- The RX (and to some extent the TX) LEDs of the RS485 transceiver (if using the one above) should light up as data goes through. If that is the case you know data is being converted from RS485 to TTL
- The Wemos D1 Mini Pro should spit out data to the MQTT broker (MQTT Spy can be useful here to see: 1. that it is connected to the wifi; 2. that it is connected to the broker). If that is the case, you know the device can communicate over MQTT
- Swap A and B - in my personal experience, if A & B are the wrong way round, the hot tub display (if you have one) will display (NO COMM) as the RS485 traffic get garbled by the esp8266_spa

# Appetiser using OpenHab...
![Example](https://github.com/cribskip/esp8266_spa/blob/master/spa_openhab.png)

# HomeAssistant integration
The system uses HomeAssistant autodiscover and should just appear in the MQTT Integration under "Esp Spa"
![Example](https://github.com/EmmanuelLM/esp8266_spa/blob/master//Hassio.png)

# TODO
- Add more documentation
- Add fault reporting
- Add more setting possibilities (filter cycles, preferences maybe)
