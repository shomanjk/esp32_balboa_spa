// Update as needed and copy to config.h

#define WIFI_SSID "xxxxxx"
#define WIFI_PASSWORD "xxxxxx"

#define MQTT_SERVER "mqtt.local"
#define MQTT_PORT 1883

#define BROKER_LOGIN ""
#define BROKER_PASS ""

#define GMT_OFFSET -14400
#define DAYLIGHT_OFFSET 0

#define AUTO_TX true

// Used by LOCAL_CLIENT - rs485 connection

#define TX485_Rx 22
#define TX485_Tx 19

// Ensure the correct serial port is used for RS485 communication
#define RS485_SERIAL_PORT Serial2