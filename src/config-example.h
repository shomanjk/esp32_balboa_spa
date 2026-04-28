// Update as needed and copy to config.h

#define WIFI_SSID "xxxxxx"
#define WIFI_PASSWORD "xxxxxx"

#define MQTT_SERVER "mqtt.local"
#define MQTT_PORT 1883

#define BROKER_LOGIN ""
#define BROKER_PASS ""

// Home Assistant MQTT Discovery (defaults also in mqttModule.h if omitted here)
// #define MQTT_HA_DISCOVERY 0   /* set to 0 to skip publishing homeassistant/.../config */
// #define MQTT_DISCOVERY_PREFIX "homeassistant"  /* broker discovery prefix */
// #define MQTT_HA_TEMP_UNIT "\xC2\xB0" "C"      /* °C — discovery uses static unit; match your tub */

// OTA controls
// Keep false for trusted-LAN setups; set true + password before broader exposure.
#define ENABLE_OTA_AUTH false
#define OTA_PASSWORD "change-me-before-enabling-auth"
// OTA transport timeout in milliseconds (applies to ArduinoOTA session)
#define OTA_TIMEOUT_MS 15000

#define GMT_OFFSET -14400
#define DAYLIGHT_OFFSET 0

#define AUTO_TX true

// Used by LOCAL_CLIENT — UART2 pins for TTL side of RS485 transceiver (see README).

// Default (generic ESP32 dev board wiring, e.g. GPIO16/17):
#define TX485_Rx 16
#define TX485_Tx 17

// RS485 UART (default ESP32: Serial2). Override in config.h if you use a different port.
#ifndef RS485_SERIAL_PORT
#define RS485_SERIAL_PORT Serial2
#endif

// ---------------------------------------------------------------------------
// M5 Atom Lite + Atomic RS485 Base (recommended tub-side stack for this repo)
// ---------------------------------------------------------------------------
// Copy the lines you need into your active config.h (only one RX/TX pair).
//
// Stack Atom Lite on the Atomic RS485 Base, UART2 per M5:
//   https://docs.m5stack.com/en/atom/Atomic%20RS485%20Base
// Arduino reference: Serial2 @ RX=22, TX=19
//   https://github.com/m5stack/M5-ProductExampleCodes/tree/master/AtomBase/AtomicRS485
//
// #undef TX485_Rx
// #undef TX485_Tx
// #define TX485_Rx 22
// #define TX485_Tx 19
// #define AUTO_TX true
//
// ---------------------------------------------------------------------------
// Alternate: M5 Atom Lite + Unit RS485 (Grove on HY2.0, not the Atomic base)
// ---------------------------------------------------------------------------
// Atom Lite Grove: Black=GND, Red=5V, Yellow=G26, White=G32
// Unit RS485 Grove: Yellow = module UART_RX, White = module UART_TX
//   https://docs.m5stack.com/en/unit/rs485
// Wire: ESP TX (G26) -> module RX (yellow); ESP RX (G32) -> module TX (white).
// If you see no frames, swap those two TTL wires.
//
// #undef TX485_Rx
// #undef TX485_Tx
// #define TX485_Rx 32
// #define TX485_Tx 26
// #define AUTO_TX true
