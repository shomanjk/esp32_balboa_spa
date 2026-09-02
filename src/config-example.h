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
// Replace both before any USB/OTA upload. PlatformIO refuses placeholder credentials on
// `pio run … -t upload` (see scripts/check_wifi_config_for_upload.py). Compile-only is fine.
// Optional: lock STA to one mesh AP (BSSID). Omit for strongest-AP selection on connect/reconnect.
// First remote OTA of the mesh STA build should leave this undefined until /api/wifi confirms the desired AP.
// #define WIFI_BSSID "aa:bb:cc:dd:ee:ff"
// Optional reconnect knobs (defaults in lib/wifiModule/wifiModule.h):
// #define WIFI_RECONNECT_INITIAL_MS 5000UL
// #define WIFI_RECONNECT_MAX_MS 60000UL
// #define WIFI_CONNECT_ATTEMPT_TIMEOUT_MS 15000UL
// Legacy WIFI_CONNECT_TIMEOUT (if set without WIFI_CONNECT_ATTEMPT_TIMEOUT_MS) maps to the async attempt timeout.

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
// HTTP liveness: periodic loopback GET /api/version; restart after consecutive probe
// failures (recovers connected-but-unreachable web stack without rebooting idle gateways).
// Omitted → on.
// #define HTTP_LIVENESS_WATCHDOG 0
// #define HTTP_LIVENESS_PROBE_INTERVAL_MS (2UL * 60UL * 1000UL)
// #define HTTP_LIVENESS_PROBE_TIMEOUT_MS 5000UL
// #define HTTP_LIVENESS_PROBE_FAIL_MAX 3
// #define HTTP_LIVENESS_MIN_UPTIME_MS (3UL * 60UL * 1000UL)

#define GMT_OFFSET -14400
#define DAYLIGHT_OFFSET 0

// After Wi-Fi/NTP is up, sync spa panel clock once per boot when drift exceeds threshold (Balboa 0x21).
// Requires correct GMT_OFFSET / DAYLIGHT_OFFSET above. Set to 0 to disable. Omitted in older config.h → off.
#define AUTO_SYNC_PANEL_CLOCK 1
// #define AUTO_SYNC_PANEL_CLOCK_THRESHOLD_MIN 2

// Used by LOCAL_CLIENT — UART2 pins for TTL side of RS485 transceiver (see README).
//
// Pin ownership:
//   - M5AtomLite-tub / M5AtomLite-tub-ota: env sets TX485_Rx=22, TX485_Tx=19, AUTO_TX — omit overrides here.
//   - M5AtomS3Lite-tub (AtomS3 Lite, not AtomS3): env sets TX485_Rx=5, TX485_Tx=6, AUTO_TX — omit overrides here.
//   - Generic envs (ESP32ota, ESP32prodOta, …): set pins / AUTO_TX in this file (defaults below).
// Migration: if your private config.h still #define's TX485_* / AUTO_TX unconditionally, M5 envs will
// redefinition-warn/error until you wrap them in #ifndef (as below) or remove those lines.

#ifndef AUTO_TX
#define AUTO_TX true
#endif

// Default (generic ESP32 dev board wiring, e.g. GPIO16/17) when the env does not -D the pins:
#ifndef TX485_Rx
#define TX485_Rx 16
#endif
#ifndef TX485_Tx
#define TX485_Tx 17
#endif

// RS485 UART (default ESP32: Serial2). Override in config.h if you use a different port.
#ifndef RS485_SERIAL_PORT
#define RS485_SERIAL_PORT Serial2
#endif

// ---------------------------------------------------------------------------
// Known M5 stacks (pins from PlatformIO env — do not redefine above)
// ---------------------------------------------------------------------------
// M5 Atom Lite + Atomic RS485 Base (M5AtomLite-tub / -ota): RX=22, TX=19
//   https://docs.m5stack.com/en/atom/Atomic%20RS485%20Base
// M5 AtomS3 Lite + Atomic RS485 Base (M5AtomS3Lite-tub): RX=5, TX=6
//   Not the screen AtomS3 / AtomS3R — https://docs.m5stack.com/en/core/AtomS3%20Lite
//   wiki: Hardware-targets
//
// ---------------------------------------------------------------------------
// Alternate wiring on a generic env (not an M5*-tub env — those own pins via build_flags)
// ---------------------------------------------------------------------------
// Alternate Atom Lite RS485 (generic env — NOT M5AtomLite-tub; that env uses 22/19 only):
// M5 Atom Lite MCU is ESP32-PICO-D4 ("ESP32 Pico" in community posts = this board).
//
// Tail485 (tail stack — same mechanical style as Atomic base, NOT Grove):
//   https://docs.m5stack.com/en/atom/tail485 — Atom G26=TX, G32=RX → TX485_Tx=26, TX485_Rx=32
//
// Unit RS485 (Grove cable — same 32/26 pins, different connector):
//   Black=GND, Red=5V, Yellow=G26, White=G32
//   https://docs.m5stack.com/en/unit/rs485
// Wire: ESP TX (G26) -> module RX (yellow); ESP RX (G32) -> module TX (white).
// If you see no frames, swap those two TTL wires.
//
// Both alternates: GENERIC PlatformIO env (ESP32ota, ESP32prodOta, …), AUTO_TX true.
// Manual DE/RE from an ESP32 GPIO is NOT supported (no RS485_DIR_PIN; AUTO_TX false
// toggles the UART TX data pin). Tail485 handles direction on-module; only TX/RX reach the Atom.
//   wiki: Hardware-targets — Atom Lite + alternate RS485 (32/26 pins)
//
// Uncomment below to override the defaults above. Use #undef first — a second
// #ifndef TX485_Rx after the defaults would silently keep 16/17.
// #undef TX485_Rx
// #undef TX485_Tx
// #undef AUTO_TX
// #define TX485_Rx 32
// #define TX485_Tx 26
// #define AUTO_TX true
