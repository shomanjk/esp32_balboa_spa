// Update as needed and copy to config.h

// Optional: override firmware repo URLs (defaults in main.h). Define before any header
// that includes main.h, or use build_flags -DFIRMWARE_REPO_OWNER=\"you\" in platformio.ini.
// #undef FIRMWARE_REPO_OWNER
// #define FIRMWARE_REPO_OWNER "myuser"
// #undef FIRMWARE_REPO_NAME
// #define FIRMWARE_REPO_NAME "esp32_balboa_spa"
// #undef FIRMWARE_REPO_DEFAULT_BRANCH
// #define FIRMWARE_REPO_DEFAULT_BRANCH "ESP32"
// Optional: branch/source tree URL for /state firmware links (defaults from owner/name/branch).
// #undef FIRMWARE_REPO_BRANCH_URL
// #define FIRMWARE_REPO_BRANCH_URL "https://github.com/myuser/esp32_balboa_spa/tree/ESP32"
// Optional: sponsor button on /state points at GitHub login (defaults to FIRMWARE_REPO_OWNER).
// #undef FIRMWARE_SPONSOR_BUTTON_SRC
// #define FIRMWARE_SPONSOR_BUTTON_SRC "https://github.com/sponsors/myuser/button"

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
// If Wi-Fi remains offline for this long, firmware self-restarts to recover.
// #define WIFI_OFFLINE_RESTART_TIMEOUT_MS (10UL * 60UL * 1000UL)
// Optional guard to avoid reboot churn during early boot.
// #define WIFI_OFFLINE_RESTART_MIN_UPTIME_MS (2UL * 60UL * 1000UL)

#define GMT_OFFSET -14400
#define DAYLIGHT_OFFSET 0

// After Wi-Fi/NTP is up, sync spa panel clock once per boot when drift exceeds threshold (Balboa 0x21).
// Requires correct GMT_OFFSET / DAYLIGHT_OFFSET above. Set to 0 to disable. Omitted in older config.h → off.
#define AUTO_SYNC_PANEL_CLOCK 1
// #define AUTO_SYNC_PANEL_CLOCK_THRESHOLD_MIN 2

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
