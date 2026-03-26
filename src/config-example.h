// Update as needed and copy to config.h

#define WIFI_SSID "xxxxxx"
#define WIFI_PASSWORD "xxxxxx"

#define MQTT_SERVER "mqtt.local"
#define MQTT_PORT 1883

#define BROKER_LOGIN ""
#define BROKER_PASS ""

#define GMT_OFFSET -14400
#define DAYLIGHT_OFFSET 0

#define AUTO_TX false

// Used by LOCAL_CLIENT — UART2 pins for TTL side of RS485 transceiver (see README).

// Default (generic ESP32 dev board wiring, e.g. GPIO16/17):
#define TX485_Rx 16
#define TX485_Tx 17

// ---------------------------------------------------------------------------
// M5 Atom Lite + M5 RS485 module (alternative to the defaults above)
// ---------------------------------------------------------------------------
// Copy the lines you need into your active config.h (only one RX/TX pair).
//
// Atom Lite Grove HY2.0 (4P): Black=GND, Red=5V, Yellow=G26, White=G32
//   https://docs.m5stack.com/en/core/ATOM%20Lite
// M5 Unit RS485 Grove: Yellow = module UART_RX, White = module UART_TX
//   https://docs.m5stack.com/en/unit/rs485
// Wire: ESP TX (G26) -> module RX (yellow); ESP RX (G32) -> module TX (white).
// If you see no frames, swap those two TTL wires.
//
// Many M5 RS485 boards handle direction automatically — prefer AUTO_TX true.
// If yours uses a separate DE/RE line, you may need AUTO_TX false and extra wiring.
//
// #undef TX485_Rx
// #undef TX485_Tx
// #define TX485_Rx 32
// #define TX485_Tx 26
// #define AUTO_TX true