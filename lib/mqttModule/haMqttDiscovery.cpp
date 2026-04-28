#include "haMqttDiscovery.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ArduinoLog.h>
#include <WiFi.h>

#include "mqttModule.h"
#include <spaMessage.h>
#include <wifiModule.h>

#include "../../src/main.h"

#if MQTT_HA_DISCOVERY

namespace
{

  /** Bits for optional HA entities (pumps, loads, diagnostics). Minimal discovery is separate. */
  enum HaEquipBit : uint32_t
  {
    HA_PUMP1 = 1u << 0,
    HA_PUMP2 = 1u << 1,
    HA_PUMP3 = 1u << 2,
    HA_PUMP4 = 1u << 3,
    HA_PUMP5 = 1u << 4,
    HA_PUMP6 = 1u << 5,
    HA_CIRC = 1u << 6,
    HA_BLOWER = 1u << 7,
    HA_LIGHT1 = 1u << 8,
    HA_LIGHT2 = 1u << 9,
    HA_MISTER = 1u << 10,
    HA_MODEL = 1u << 11,
    HA_SOFTWARE = 1u << 12,
  };

  /** Every optional discovery entity (not in minimal set). Used to retract stale retained configs from the broker. */
  static constexpr uint32_t ALL_EQUIPMENT_BITS =
      HA_PUMP1 | HA_PUMP2 | HA_PUMP3 | HA_PUMP4 | HA_PUMP5 | HA_PUMP6 | HA_CIRC | HA_BLOWER | HA_LIGHT1 |
      HA_LIGHT2 | HA_MISTER | HA_MODEL | HA_SOFTWARE;

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
    dev["configuration_url"] = String("http://") + String(gatewayName) + ".local/status";
  }

  String stateTopic(const char *group, const char *field)
  {
    return mqttTopic + String(group) + "/" + field;
  }

  void buildObjectId(char *out, size_t outLen, const char *objectSuffix)
  {
    snprintf(out, outLen, "%s_%s", gatewayName, objectSuffix);
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

  void retractDiscoveryConfig(const char *haComponent, const char *objectSuffix)
  {
    char objectId[48];
    buildObjectId(objectId, sizeof(objectId), objectSuffix);
    char topic[192];
    snprintf(topic, sizeof(topic), "%s/%s/%s/config", MQTT_DISCOVERY_PREFIX, haComponent, objectId);
    mqtt.publish(topic, "", true);
    mqtt.loop();
  }

  void publishSensor(const char *macSlugStr, const char *objectSuffix, const char *friendlyName,
                     const char *group, const char *field, const char *deviceClass,
                     const char *unit, const char *stateClass, const char *entityCategory,
                     const char *icon = nullptr)
  {
    char objectId[48];
    buildObjectId(objectId, sizeof(objectId), objectSuffix);

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
    if (icon && icon[0])
      root["icon"] = icon;

    publishDoc("sensor", objectId, doc);
  }

  void publishBinarySensor(const char *macSlugStr, const char *objectSuffix, const char *friendlyName,
                           const char *group, const char *field, const char *icon = nullptr,
                           const char *payloadOn = "On", const char *payloadOff = "Off")
  {
    char objectId[48];
    buildObjectId(objectId, sizeof(objectId), objectSuffix);

    char uniqueId[64];
    snprintf(uniqueId, sizeof(uniqueId), "%s_%s", macSlugStr, objectSuffix);

    StaticJsonDocument<1024> doc;
    JsonObject root = doc.to<JsonObject>();
    root["name"] = friendlyName;
    root["unique_id"] = uniqueId;
    root["object_id"] = objectId;
    root["state_topic"] = stateTopic(group, field);
    root["payload_on"] = payloadOn;
    root["payload_off"] = payloadOff;
    addAvailability(root, mqttTopic);
    addDevice(root, macSlugStr);
    if (icon && icon[0])
      root["icon"] = icon;

    publishDoc("binary_sensor", objectId, doc);
  }

  void publishEnumSensor(const char *macSlugStr, const char *objectSuffix, const char *friendlyName,
                         const char *group, const char *field, const char *const *options,
                         size_t optionCount, const char *entityCategory = nullptr, const char *icon = nullptr)
  {
    char objectId[48];
    buildObjectId(objectId, sizeof(objectId), objectSuffix);

    char uniqueId[64];
    snprintf(uniqueId, sizeof(uniqueId), "%s_%s", macSlugStr, objectSuffix);

    StaticJsonDocument<1024> doc;
    JsonObject root = doc.to<JsonObject>();
    root["name"] = friendlyName;
    root["unique_id"] = uniqueId;
    root["object_id"] = objectId;
    root["state_topic"] = stateTopic(group, field);
    root["device_class"] = "enum";
    JsonArray opts = root.createNestedArray("options");
    for (size_t i = 0; i < optionCount; i++)
      opts.add(options[i]);
    addAvailability(root, mqttTopic);
    addDevice(root, macSlugStr);
    if (entityCategory && entityCategory[0])
      root["entity_category"] = entityCategory;
    if (icon && icon[0])
      root["icon"] = icon;

    publishDoc("sensor", objectId, doc);
  }

  /** Same rules as spaWebServer: pump/light two-bit 0 = not installed. */
  uint32_t computeDesiredEquipmentMask()
  {
    uint32_t d = 0;
    if (spaConfigurationData.lastUpdate != 0)
    {
      if (spaConfigurationData.pump1 != 0)
        d |= HA_PUMP1;
      if (spaConfigurationData.pump2 != 0)
        d |= HA_PUMP2;
      if (spaConfigurationData.pump3 != 0)
        d |= HA_PUMP3;
      if (spaConfigurationData.pump4 != 0)
        d |= HA_PUMP4;
      if (spaConfigurationData.pump5 != 0)
        d |= HA_PUMP5;
      if (spaConfigurationData.pump6 != 0)
        d |= HA_PUMP6;
      if (spaConfigurationData.circulationPump)
        d |= HA_CIRC;
      if (spaConfigurationData.blower)
        d |= HA_BLOWER;
      if (spaConfigurationData.light1 != 0)
        d |= HA_LIGHT1;
      if (spaConfigurationData.light2 != 0)
        d |= HA_LIGHT2;
      if (spaConfigurationData.mister != 0)
        d |= HA_MISTER;
    }
    if (spaInformationData.lastUpdate != 0)
    {
      d |= HA_MODEL;
      d |= HA_SOFTWARE;
    }
    return d;
  }

  void retractEquipmentBit(uint32_t bit, const char *macSlugStr)
  {
    (void)macSlugStr;
    switch (bit)
    {
    case HA_PUMP1:
      retractDiscoveryConfig("sensor", "pump1");
      break;
    case HA_PUMP2:
      retractDiscoveryConfig("sensor", "pump2");
      break;
    case HA_PUMP3:
      retractDiscoveryConfig("sensor", "pump3");
      break;
    case HA_PUMP4:
      retractDiscoveryConfig("sensor", "pump4");
      break;
    case HA_PUMP5:
      retractDiscoveryConfig("sensor", "pump5");
      break;
    case HA_PUMP6:
      retractDiscoveryConfig("sensor", "pump6");
      break;
    case HA_CIRC:
      retractDiscoveryConfig("binary_sensor", "circ");
      break;
    case HA_BLOWER:
      retractDiscoveryConfig("binary_sensor", "blower");
      break;
    case HA_LIGHT1:
      retractDiscoveryConfig("binary_sensor", "light1");
      break;
    case HA_LIGHT2:
      retractDiscoveryConfig("binary_sensor", "light2");
      break;
    case HA_MISTER:
      retractDiscoveryConfig("sensor", "mister");
      retractDiscoveryConfig("binary_sensor", "mister");
      break;
    case HA_MODEL:
      retractDiscoveryConfig("sensor", "model");
      break;
    case HA_SOFTWARE:
      retractDiscoveryConfig("sensor", "software_id");
      break;
    default:
      break;
    }
  }

  void publishEquipmentBit(uint32_t bit, const char *macSlugStr)
  {
    switch (bit)
    {
    case HA_PUMP1:
      publishSensor(macSlugStr, "pump1", "Spa pump 1", "status", "pump1", nullptr, nullptr, nullptr, nullptr, "mdi:pump");
      break;
    case HA_PUMP2:
      publishSensor(macSlugStr, "pump2", "Spa pump 2", "status", "pump2", nullptr, nullptr, nullptr, nullptr, "mdi:pump");
      break;
    case HA_PUMP3:
      publishSensor(macSlugStr, "pump3", "Spa pump 3", "status", "pump3", nullptr, nullptr, nullptr, nullptr, "mdi:pump");
      break;
    case HA_PUMP4:
      publishSensor(macSlugStr, "pump4", "Spa pump 4", "status", "pump4", nullptr, nullptr, nullptr, nullptr, "mdi:pump");
      break;
    case HA_PUMP5:
      publishSensor(macSlugStr, "pump5", "Spa pump 5", "status", "pump5", nullptr, nullptr, nullptr, nullptr, "mdi:pump");
      break;
    case HA_PUMP6:
      publishSensor(macSlugStr, "pump6", "Spa pump 6", "status", "pump6", nullptr, nullptr, nullptr, nullptr, "mdi:pump");
      break;
    case HA_CIRC:
      publishBinarySensor(macSlugStr, "circ", "Spa circulation pump", "status", "circ", "mdi:pump");
      break;
    case HA_BLOWER:
      publishBinarySensor(macSlugStr, "blower", "Spa blower", "status", "blower");
      break;
    case HA_LIGHT1:
      publishBinarySensor(macSlugStr, "light1", "Spa light 1", "status", "light1");
      break;
    case HA_LIGHT2:
      publishBinarySensor(macSlugStr, "light2", "Spa light 2", "status", "light2");
      break;
    case HA_MISTER:
      retractDiscoveryConfig("sensor", "mister");
      publishBinarySensor(macSlugStr, "mister", "Spa mister", "status", "mister");
      break;
    case HA_MODEL:
      publishSensor(macSlugStr, "model", "Spa controller model", "information", "model", nullptr, nullptr, nullptr, "diagnostic");
      break;
    case HA_SOFTWARE:
      publishSensor(macSlugStr, "software_id", "Spa software ID", "information", "softwareID", nullptr, nullptr, nullptr, "diagnostic");
      break;
    default:
      break;
    }
  }

  void publishMinimalDiscovery(const char *macSlugStr)
  {
    static const char *HEATING_STATE_OPTIONS[] = {"Idle / not heating", "Heating (active)", "Heating (alternate stage)", "Reserved"};
    static const char *SPA_STATE_OPTIONS[] = {"Running", "Initializing", "Hold Mode", "A/B Temps ON", "Test Mode"};
    static const char *INIT_MODE_OPTIONS[] = {"Idle", "Priming Mode", "Fault", "Reminder", "Stage 1", "Stage 2", "Stage 3"};
    static const char *HEATING_MODE_OPTIONS[] = {"Ready", "Rest", "Ready in Rest"};
    static const char *FILTER_MODE_OPTIONS[] = {"Off", "Cycle 1", "Cycle 2", "Cycle 1 & 2"};
    static const char *TEMP_RANGE_OPTIONS[] = {"Low Range", "High Range"};

    // Retract retained discovery that changed platform/type.
    retractDiscoveryConfig("sensor", "panel_locked");
    retractDiscoveryConfig("sensor", "settings_lock");
    retractDiscoveryConfig("sensor", "spa_time");

    publishSensor(macSlugStr, "current_temp", "Spa current temperature", "status", "currentTemp", "temperature", MQTT_HA_TEMP_UNIT, nullptr, nullptr, nullptr);
    publishSensor(macSlugStr, "set_temp", "Spa set temperature", "status", "setTemp", "temperature", MQTT_HA_TEMP_UNIT, nullptr, nullptr, nullptr);
    publishSensor(macSlugStr, "low_set_temp", "Spa low set temperature", "status", "lowSetTemp", "temperature", MQTT_HA_TEMP_UNIT, nullptr, nullptr, nullptr);
    publishSensor(macSlugStr, "high_set_temp", "Spa high set temperature", "status", "highSetTemp", "temperature", MQTT_HA_TEMP_UNIT, nullptr, nullptr, nullptr);
    publishSensor(macSlugStr, "sensor_a", "Spa sensor A", "status", "sensorA", "temperature", MQTT_HA_TEMP_UNIT, nullptr, nullptr, nullptr);
    publishSensor(macSlugStr, "sensor_b", "Spa sensor B", "status", "sensorB", "temperature", MQTT_HA_TEMP_UNIT, nullptr, nullptr, nullptr);

    publishEnumSensor(macSlugStr, "heating_state", "Spa heating state", "status", "heatingState",
                      HEATING_STATE_OPTIONS, sizeof(HEATING_STATE_OPTIONS) / sizeof(HEATING_STATE_OPTIONS[0]));
    publishEnumSensor(macSlugStr, "spa_state", "Spa state", "status", "spaState", SPA_STATE_OPTIONS,
                      sizeof(SPA_STATE_OPTIONS) / sizeof(SPA_STATE_OPTIONS[0]), nullptr, "mdi:hot-tub");
    publishEnumSensor(macSlugStr, "init_mode", "Spa init mode", "status", "initMode", INIT_MODE_OPTIONS,
                      sizeof(INIT_MODE_OPTIONS) / sizeof(INIT_MODE_OPTIONS[0]));
    publishEnumSensor(macSlugStr, "heating_mode", "Spa heating mode", "status", "heatingMode",
                      HEATING_MODE_OPTIONS, sizeof(HEATING_MODE_OPTIONS) / sizeof(HEATING_MODE_OPTIONS[0]));
    publishEnumSensor(macSlugStr, "filter_mode", "Spa filter mode", "status", "filterMode",
                      FILTER_MODE_OPTIONS, sizeof(FILTER_MODE_OPTIONS) / sizeof(FILTER_MODE_OPTIONS[0]), nullptr, "mdi:sync");
    publishEnumSensor(macSlugStr, "temp_range", "Spa temperature range", "status", "tempRange",
                      TEMP_RANGE_OPTIONS, sizeof(TEMP_RANGE_OPTIONS) / sizeof(TEMP_RANGE_OPTIONS[0]), nullptr,
                      "mdi:thermometer-lines");
    publishBinarySensor(macSlugStr, "panel_locked", "Spa panel locked", "status", "panelLocked", nullptr, "Locked", "Unlocked");
    publishBinarySensor(macSlugStr, "settings_lock", "Spa settings lock", "status", "settingsLock", nullptr, "Locked", "Unlocked");

    publishSensor(macSlugStr, "temp_scale", "Spa temperature scale", "status", "tempScale", nullptr, nullptr, nullptr, "diagnostic", nullptr);
  }

} // namespace

void publishHomeAssistantDiscovery()
{
  if (!mqtt.connected())
    return;

  char macStr[20];
  macSlug(macStr, sizeof(macStr));

  Log.notice(F("[HA discovery]: minimal MQTT discovery (prefix=%s)" CR), MQTT_DISCOVERY_PREFIX);
  publishMinimalDiscovery(macStr);
  publishHomeAssistantDiscoveryExpanded();
}

void publishHomeAssistantDiscoveryExpanded()
{
  if (!mqtt.connected())
    return;

  char macStr[20];
  macSlug(macStr, sizeof(macStr));

  const uint32_t desired = computeDesiredEquipmentMask();
  /* Retract every optional slot we are not publishing, including stale retained discovery from older sessions
   * or firmware (previously we only retracted bits published in-RAM this boot, so HA kept ghost entities). */
  const uint32_t retract = ALL_EQUIPMENT_BITS & ~desired;

  for (uint32_t b = HA_PUMP1; b <= HA_SOFTWARE; b <<= 1)
  {
    if (retract & b)
    {
      retractEquipmentBit(b, macStr);
      Log.verbose(F("[HA discovery]: retract equipment bit 0x%lx" CR), (unsigned long)b);
    }
  }

  for (uint32_t b = HA_PUMP1; b <= HA_SOFTWARE; b <<= 1)
  {
    if (desired & b)
      publishEquipmentBit(b, macStr);
  }

  if (desired != 0)
    Log.notice(F("[HA discovery]: equipment discovery mask 0x%lx (config=%lu info=%lu)" CR),
               (unsigned long)desired,
               (unsigned long)spaConfigurationData.lastUpdate,
               (unsigned long)spaInformationData.lastUpdate);
}

#else

void publishHomeAssistantDiscovery()
{
}

void publishHomeAssistantDiscoveryExpanded()
{
}

#endif // MQTT_HA_DISCOVERY
