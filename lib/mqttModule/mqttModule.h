#ifndef MQTT_MODULE_H
#define MQTT_MODULE_H

#include <PubSubClient.h>
#include <WiFi.h> // Add this line to include the WiFi library

#include "../../src/config.h" // Default passwords and SSID

#ifndef MQTT_SERVER
#warning "MQTT_SERVER not defined, please define in config.h"
#define MQTT_SERVER "mqtt.local"
#endif

#ifndef MQTT_PORT
#warning "MQTT_PORT not defined, please define in config.h"
#define MQTT_PORT 1883
#endif

#ifndef BROKER_LOGIN
#warning "BROKER_LOGIN not defined, please define in config.h"
#define BROKER_LOGIN ""
#endif

#ifndef BROKER_PASS
#warning "BROKER_LOGIN not defined, please define in config.h"
#define BROKER_PASS ""
#endif

/** Home Assistant MQTT Discovery (retained homeassistant/.../config). Set to 0 in config.h to disable. */
#ifndef MQTT_HA_DISCOVERY
#define MQTT_HA_DISCOVERY 1
#endif

#ifndef MQTT_DISCOVERY_PREFIX
#define MQTT_DISCOVERY_PREFIX "homeassistant"
#endif

/** Static unit for HA temperature sensors (see README); tub °C vs °F is not auto-detected in discovery. */
#ifndef MQTT_HA_TEMP_UNIT
#define MQTT_HA_TEMP_UNIT "\xC2\xB0" "F"
#endif

#define publishDebug(...) mqtt.publish((mqttTopic + "debug/message").c_str(), __VA_ARGS__);
#define publishError(...) mqtt.publish((mqttTopic + "debug/error").c_str(), __VA_ARGS__);
#define publishNodeStatus(topic, ...) mqtt.publish((mqttTopic + "node/" + topic).c_str(), __VA_ARGS__);

extern String mqttTopic;

extern WiFiClient wifiClient;
extern PubSubClient mqtt;

void mqttModuleSetup();
void mqttModuleLoop();

#endif