#include "haMqttDiscovery.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ArduinoLog.h>
#include <WiFi.h>

#include "mqttModule.h"
#include <wifiModule.h>

#include "../../src/main.h"

#if MQTT_HA_DISCOVERY

namespace
{

  /** Strip colons from WiFi.macAddress() for stable unique_id prefix. */
  void macSlug(char *out, size_t outLen)
  {
    String m = WiFi.macAddress();
    size_t j = 0;
    for (unsigned i = 0; i < m.length() && j + 1 < outLen; i++)
    {
      if (m[i] != ':')
        out[j++] = m[i];
    }
    out[j] = '\0';
  }

  void addAvailability(JsonObject root, const String &base)
  {
    root["availability_topic"] = base + "node/state";
    root["payload_available"] = "ON";
    root["payload_not_available"] = "OFF";
  }

  void addDevice(JsonObject root, const char *macSlugStr)
  {
    JsonObject dev = root.createNestedObject("device");
    JsonArray ids = dev.createNestedArray("identifiers");
    ids.add("esp32_balboa_spa");
    ids.add(macSlugStr);
    ids.add(gatewayName);
    dev["name"] = "Balboa Spa";
    dev["model"] = "ESP32 Balboa Gateway";
    dev["sw_version"] = VERSION;
    dev["manufacturer"] = "esp32_balboa_spa";
  }

  String stateTopic(const char *group, const char *field)
  {
    return mqttTopic + String(group) + "/" + field;
  }

  bool publishDoc(const char *haComponent, const char *objectId, const JsonDocument &doc)
  {
    char topic[192];
    snprintf(topic, sizeof(topic), "%s/%s/%s/config", MQTT_DISCOVERY_PREFIX, haComponent, objectId);

    char payload[1536];
    size_t n = serializeJson(doc, payload, sizeof(payload));
    if (n == 0 || n >= sizeof(payload))
    {
      Log.error(F("[HA discovery]: JSON too large or empty for %s" CR), topic);
      return false;
    }
    bool ok = mqtt.publish(topic, (const uint8_t *)payload, (unsigned int)n, true);
    if (!ok)
      Log.warning(F("[HA discovery]: publish failed for %s" CR), topic);
    mqtt.loop();
    return ok;
  }

  void publishSensor(const char *macSlugStr, const char *objectSuffix, const char *friendlyName,
                     const char *group, const char *field, const char *deviceClass,
                     const char *unit, const char *stateClass, const char *entityCategory)
  {
    char objectId[48];
    snprintf(objectId, sizeof(objectId), "%s_%s", gatewayName, objectSuffix);

    char uniqueId[64];
    snprintf(uniqueId, sizeof(uniqueId), "%s_%s", macSlugStr, objectSuffix);

    StaticJsonDocument<1024> doc;
    JsonObject root = doc.to<JsonObject>();
    root["name"] = friendlyName;
    root["unique_id"] = uniqueId;
    root["object_id"] = objectId;
    root["state_topic"] = stateTopic(group, field);
    addAvailability(root, mqttTopic);
    addDevice(root, macSlugStr);
    if (deviceClass && deviceClass[0])
      root["device_class"] = deviceClass;
    if (unit && unit[0])
      root["unit_of_measurement"] = unit;
    if (stateClass && stateClass[0])
      root["state_class"] = stateClass;
    if (entityCategory && entityCategory[0])
      root["entity_category"] = entityCategory;

    publishDoc("sensor", objectId, doc);
  }

  void publishBinarySensor(const char *macSlugStr, const char *objectSuffix, const char *friendlyName,
                           const char *group, const char *field)
  {
    char objectId[48];
    snprintf(objectId, sizeof(objectId), "%s_%s", gatewayName, objectSuffix);

    char uniqueId[64];
    snprintf(uniqueId, sizeof(uniqueId), "%s_%s", macSlugStr, objectSuffix);

    StaticJsonDocument<1024> doc;
    JsonObject root = doc.to<JsonObject>();
    root["name"] = friendlyName;
    root["unique_id"] = uniqueId;
    root["object_id"] = objectId;
    root["state_topic"] = stateTopic(group, field);
    root["payload_on"] = "On";
    root["payload_off"] = "Off";
    addAvailability(root, mqttTopic);
    addDevice(root, macSlugStr);

    publishDoc("binary_sensor", objectId, doc);
  }

} // namespace

void publishHomeAssistantDiscovery()
{
  if (!mqtt.connected())
    return;

  char macStr[20];
  macSlug(macStr, sizeof(macStr));

  Log.notice(F("[HA discovery]: publishing MQTT discovery (prefix=%s)" CR), MQTT_DISCOVERY_PREFIX);

  // Temperature-related (unit is compile-time / config; see README)
  publishSensor(macStr, "current_temp", "Spa current temperature", "status", "currentTemp", "temperature", MQTT_HA_TEMP_UNIT, nullptr, nullptr);
  publishSensor(macStr, "set_temp", "Spa set temperature", "status", "setTemp", "temperature", MQTT_HA_TEMP_UNIT, nullptr, nullptr);
  publishSensor(macStr, "low_set_temp", "Spa low set temperature", "status", "lowSetTemp", "temperature", MQTT_HA_TEMP_UNIT, nullptr, nullptr);
  publishSensor(macStr, "high_set_temp", "Spa high set temperature", "status", "highSetTemp", "temperature", MQTT_HA_TEMP_UNIT, nullptr, nullptr);
  publishSensor(macStr, "sensor_a", "Spa sensor A", "status", "sensorA", "temperature", MQTT_HA_TEMP_UNIT, nullptr, nullptr);
  publishSensor(macStr, "sensor_b", "Spa sensor B", "status", "sensorB", "temperature", MQTT_HA_TEMP_UNIT, nullptr, nullptr);

  publishSensor(macStr, "heating_state", "Spa heating state", "status", "heatingState", nullptr, nullptr, "measurement", nullptr);

  publishSensor(macStr, "spa_state", "Spa state", "status", "spaState", nullptr, nullptr, nullptr, nullptr);
  publishSensor(macStr, "init_mode", "Spa init mode", "status", "initMode", nullptr, nullptr, nullptr, nullptr);
  publishSensor(macStr, "heating_mode", "Spa heating mode", "status", "heatingMode", nullptr, nullptr, nullptr, nullptr);
  publishSensor(macStr, "filter_mode", "Spa filter mode", "status", "filterMode", nullptr, nullptr, nullptr, nullptr);
  publishSensor(macStr, "temp_range", "Spa temperature range", "status", "tempRange", nullptr, nullptr, nullptr, nullptr);
  publishSensor(macStr, "panel_locked", "Spa panel locked", "status", "panelLocked", nullptr, nullptr, nullptr, nullptr);
  publishSensor(macStr, "settings_lock", "Spa settings lock", "status", "settingsLock", nullptr, nullptr, nullptr, nullptr);

  publishSensor(macStr, "pump1", "Spa pump 1", "status", "pump1", nullptr, nullptr, nullptr, nullptr);
  publishSensor(macStr, "pump2", "Spa pump 2", "status", "pump2", nullptr, nullptr, nullptr, nullptr);
  publishSensor(macStr, "pump3", "Spa pump 3", "status", "pump3", nullptr, nullptr, nullptr, nullptr);
  publishSensor(macStr, "pump4", "Spa pump 4", "status", "pump4", nullptr, nullptr, nullptr, nullptr);
  publishSensor(macStr, "pump5", "Spa pump 5", "status", "pump5", nullptr, nullptr, nullptr, nullptr);
  publishSensor(macStr, "pump6", "Spa pump 6", "status", "pump6", nullptr, nullptr, nullptr, nullptr);

  publishSensor(macStr, "spa_time", "Spa clock", "status", "time", nullptr, nullptr, nullptr, nullptr);
  publishSensor(macStr, "temp_scale", "Spa temperature scale", "status", "tempScale", nullptr, nullptr, nullptr, "diagnostic");

  // Last writer on topic status/mister is numeric (see spaMqttMessage.cpp)
  publishSensor(macStr, "mister", "Spa mister", "status", "mister", nullptr, nullptr, nullptr, nullptr);

  publishBinarySensor(macStr, "circ", "Spa circulation pump", "status", "circ");
  publishBinarySensor(macStr, "blower", "Spa blower", "status", "blower");
  publishBinarySensor(macStr, "light1", "Spa light 1", "status", "light1");
  publishBinarySensor(macStr, "light2", "Spa light 2", "status", "light2");

  // Phase 1b: controller information (diagnostic)
  publishSensor(macStr, "model", "Spa controller model", "information", "model", nullptr, nullptr, nullptr, "diagnostic");
  publishSensor(macStr, "software_id", "Spa software ID", "information", "softwareID", nullptr, nullptr, nullptr, "diagnostic");

  Log.notice(F("[HA discovery]: finished" CR));
}

#else

void publishHomeAssistantDiscovery()
{
}

#endif // MQTT_HA_DISCOVERY
