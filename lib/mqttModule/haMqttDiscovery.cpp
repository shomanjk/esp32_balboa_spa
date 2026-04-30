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

  String commandTopic(const char *suffix)
  {
    return mqttTopic + "cmd/" + String(suffix);
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

  void publishSwitch(const char *macSlugStr, const char *objectSuffix, const char *friendlyName,
                     const char *group, const char *field, const char *commandSuffix,
                     const char *icon = nullptr, const char *payloadOn = "On", const char *payloadOff = "Off")
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
    root["command_topic"] = commandTopic(commandSuffix);
    root["payload_on"] = payloadOn;
    root["payload_off"] = payloadOff;
    addAvailability(root, mqttTopic);
    addDevice(root, macSlugStr);
    if (icon && icon[0])
      root["icon"] = icon;
    publishDoc("switch", objectId, doc);
  }

  void publishLight(const char *macSlugStr, const char *objectSuffix, const char *friendlyName,
                    const char *group, const char *field, const char *commandSuffix,
                    const char *icon = nullptr, const char *payloadOn = "On", const char *payloadOff = "Off")
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
    root["command_topic"] = commandTopic(commandSuffix);
    root["payload_on"] = payloadOn;
    root["payload_off"] = payloadOff;
    addAvailability(root, mqttTopic);
    addDevice(root, macSlugStr);
    if (icon && icon[0])
      root["icon"] = icon;
    publishDoc("light", objectId, doc);
  }

  void publishOneSpeedPumpSwitch(const char *macSlugStr, const char *objectSuffix, const char *friendlyName,
                                 const char *group, const char *field, const char *commandSuffix,
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
    root["command_topic"] = commandTopic(commandSuffix);
    root["payload_on"] = "on";
    root["payload_off"] = "off";
    root["state_on"] = "ON";
    root["state_off"] = "OFF";
    // One-speed pumps may still report "Low" (or occasionally "High"/"On" on some packs); normalize all to ON.
    root["value_template"] = "{{ 'ON' if value in ['Low', 'High', 'On'] else 'OFF' }}";
    addAvailability(root, mqttTopic);
    addDevice(root, macSlugStr);
    if (icon && icon[0])
      root["icon"] = icon;
    publishDoc("switch", objectId, doc);
  }

  void publishSelect(const char *macSlugStr, const char *objectSuffix, const char *friendlyName,
                     const char *group, const char *field, const char *commandSuffix,
                     const char *const *options, size_t optionCount, const char *icon = nullptr)
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
    root["command_topic"] = commandTopic(commandSuffix);
    JsonArray opts = root.createNestedArray("options");
    for (size_t i = 0; i < optionCount; i++)
      opts.add(options[i]);
    addAvailability(root, mqttTopic);
    addDevice(root, macSlugStr);
    if (icon && icon[0])
      root["icon"] = icon;
    publishDoc("select", objectId, doc);
  }

  void publishButton(const char *macSlugStr, const char *objectSuffix, const char *friendlyName,
                     const char *commandSuffix, const char *payloadPress, const char *icon = nullptr)
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
    root["command_topic"] = commandTopic(commandSuffix);
    root["payload_press"] = payloadPress;
    addAvailability(root, mqttTopic);
    addDevice(root, macSlugStr);
    if (icon && icon[0])
      root["icon"] = icon;
    publishDoc("button", objectId, doc);
  }

  void publishClimate(const char *macSlugStr)
  {
    char objectId[48];
    buildObjectId(objectId, sizeof(objectId), "spa_controls");
    char uniqueId[64];
    snprintf(uniqueId, sizeof(uniqueId), "%s_%s", macSlugStr, "spa_controls");

    StaticJsonDocument<1536> doc;
    JsonObject root = doc.to<JsonObject>();
    root["name"] = "Spa controls";
    root["unique_id"] = uniqueId;
    root["object_id"] = objectId;
    root["temperature_state_topic"] = stateTopic("status", "setTemp");
    root["temperature_command_topic"] = commandTopic("setTemp");
    root["current_temperature_topic"] = stateTopic("status", "currentTemp");
    root["mode_state_topic"] = stateTopic("status", "heatingMode");
    root["mode_command_topic"] = commandTopic("mode");
    root["mode_state_template"] = "{{ 'off' if value == 'Rest' else 'heat' }}";
    JsonArray modes = root.createNestedArray("modes");
    modes.add("heat");
    modes.add("off");
    root["preset_mode_state_topic"] = stateTopic("status", "tempRange");
    root["preset_mode_command_topic"] = commandTopic("preset");
    JsonArray presets = root.createNestedArray("preset_modes");
    presets.add("Low Range");
    presets.add("High Range");
    const String configuredTempUnit = String(MQTT_HA_TEMP_UNIT);
    const bool isCelsius = configuredTempUnit.endsWith("C") || configuredTempUnit.endsWith("c");
    root["temperature_unit"] = isCelsius ? "C" : "F";
    if (isCelsius)
    {
      root["min_temp"] = 10;
      root["max_temp"] = 40;
      root["temp_step"] = 0.5;
    }
    else
    {
      root["min_temp"] = 50;
      root["max_temp"] = 104;
      root["temp_step"] = 1.0;
    }
    addAvailability(root, mqttTopic);
    addDevice(root, macSlugStr);
    publishDoc("climate", objectId, doc);
  }

  void publishCommandResultSensor(const char *macSlugStr)
  {
    char objectId[48];
    buildObjectId(objectId, sizeof(objectId), "last_command_result");
    char uniqueId[64];
    snprintf(uniqueId, sizeof(uniqueId), "%s_%s", macSlugStr, "last_command_result");

    StaticJsonDocument<1024> doc;
    JsonObject root = doc.to<JsonObject>();
    root["name"] = "Spa last command result";
    root["unique_id"] = uniqueId;
    root["object_id"] = objectId;
    root["state_topic"] = commandTopic("result");
    root["value_template"] = "{{ value_json.reason }}";
    root["json_attributes_topic"] = commandTopic("result");
    root["entity_category"] = "diagnostic";
    addAvailability(root, mqttTopic);
    addDevice(root, macSlugStr);
    publishDoc("sensor", objectId, doc);
  }

  void retractWritableOverlapConfigs()
  {
    retractDiscoveryConfig("sensor", "set_temp");
    retractDiscoveryConfig("sensor", "temp_range");
    retractDiscoveryConfig("sensor", "heating_mode");
    retractDiscoveryConfig("sensor", "pump1");
    retractDiscoveryConfig("sensor", "pump2");
    retractDiscoveryConfig("sensor", "pump3");
    retractDiscoveryConfig("sensor", "pump4");
    retractDiscoveryConfig("sensor", "pump5");
    retractDiscoveryConfig("sensor", "pump6");
    retractDiscoveryConfig("binary_sensor", "light1");
    retractDiscoveryConfig("binary_sensor", "light2");
    retractDiscoveryConfig("switch", "light1");
    retractDiscoveryConfig("switch", "light2");
    retractDiscoveryConfig("binary_sensor", "blower");
    retractDiscoveryConfig("binary_sensor", "mister");
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
      retractDiscoveryConfig("switch", "blower");
      break;
    case HA_LIGHT1:
      retractDiscoveryConfig("binary_sensor", "light1");
      retractDiscoveryConfig("switch", "light1");
      retractDiscoveryConfig("light", "light1");
      break;
    case HA_LIGHT2:
      retractDiscoveryConfig("binary_sensor", "light2");
      retractDiscoveryConfig("switch", "light2");
      retractDiscoveryConfig("light", "light2");
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
      // Writable light entities are published in publishWritableEntities().
      // Keep expanded equipment publish from creating a second entity platform for the same object.
      retractDiscoveryConfig("binary_sensor", "light1");
      retractDiscoveryConfig("switch", "light1");
      break;
    case HA_LIGHT2:
      // Writable light entities are published in publishWritableEntities().
      // Keep expanded equipment publish from creating a second entity platform for the same object.
      retractDiscoveryConfig("binary_sensor", "light2");
      retractDiscoveryConfig("switch", "light2");
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

  uint8_t pumpSpeedConfigForBit(uint32_t bit)
  {
    switch (bit)
    {
    case HA_PUMP1:
      return spaConfigurationData.pump1;
    case HA_PUMP2:
      return spaConfigurationData.pump2;
    case HA_PUMP3:
      return spaConfigurationData.pump3;
    case HA_PUMP4:
      return spaConfigurationData.pump4;
    case HA_PUMP5:
      return spaConfigurationData.pump5;
    case HA_PUMP6:
      return spaConfigurationData.pump6;
    default:
      return 0;
    }
  }

  void publishWritablePumpBit(uint32_t bit, const char *macSlugStr)
  {
    const char *const kPumpTriOptions[] = {"Off", "Low", "High"};
    const char *suffix = "";
    const char *name = "";
    const char *field = "";
    const char *cmd = "";

    switch (bit)
    {
    case HA_PUMP1:
      suffix = "pump1";
      name = "Spa pump 1";
      field = "pump1";
      cmd = "button/4";
      break;
    case HA_PUMP2:
      suffix = "pump2";
      name = "Spa pump 2";
      field = "pump2";
      cmd = "button/5";
      break;
    case HA_PUMP3:
      suffix = "pump3";
      name = "Spa pump 3";
      field = "pump3";
      cmd = "button/6";
      break;
    case HA_PUMP4:
      suffix = "pump4";
      name = "Spa pump 4";
      field = "pump4";
      cmd = "button/7";
      break;
    case HA_PUMP5:
      suffix = "pump5";
      name = "Spa pump 5";
      field = "pump5";
      cmd = "button/8";
      break;
    case HA_PUMP6:
      suffix = "pump6";
      name = "Spa pump 6";
      field = "pump6";
      cmd = "button/9";
      break;
    default:
      return;
    }

    const uint8_t speedConfig = pumpSpeedConfigForBit(bit);
    retractDiscoveryConfig("switch", suffix);
    retractDiscoveryConfig("select", suffix);
    if (speedConfig <= 1)
    {
      publishOneSpeedPumpSwitch(macSlugStr, suffix, name, "status", field, cmd, "mdi:pump");
    }
    else
    {
      publishSelect(macSlugStr, suffix, name, "status", field, cmd, kPumpTriOptions, sizeof(kPumpTriOptions) / sizeof(kPumpTriOptions[0]), "mdi:pump");
    }
  }

  void publishWritableEntities(const char *macSlugStr, uint32_t desired)
  {
    retractWritableOverlapConfigs();
    publishClimate(macSlugStr);
    if (desired & HA_LIGHT1)
      publishLight(macSlugStr, "light1", "Spa light 1", "status", "light1", "button/17", "mdi:lightbulb", "On", "Off");
    else
      retractDiscoveryConfig("light", "light1");
    if (desired & HA_LIGHT2)
      publishLight(macSlugStr, "light2", "Spa light 2", "status", "light2", "button/18", "mdi:lightbulb", "On", "Off");
    else
      retractDiscoveryConfig("light", "light2");
    publishSwitch(macSlugStr, "blower", "Spa blower", "status", "blower", "button/12", nullptr, "On", "Off");
    publishSwitch(macSlugStr, "mister", "Spa mister", "status", "mister", "button/14", nullptr, "On", "Off");
    publishButton(macSlugStr, "sync_panel_time", "Spa sync panel clock", "syncTime", "1", "mdi:clock-check");
    publishCommandResultSensor(macSlugStr);

    for (uint32_t b = HA_PUMP1; b <= HA_PUMP6; b <<= 1)
    {
      if (desired & b)
      {
        publishWritablePumpBit(b, macSlugStr);
      }
      else
      {
        retractDiscoveryConfig("switch", b == HA_PUMP1 ? "pump1" : b == HA_PUMP2 ? "pump2"
                                        : b == HA_PUMP3   ? "pump3"
                                        : b == HA_PUMP4   ? "pump4"
                                        : b == HA_PUMP5   ? "pump5"
                                                          : "pump6");
        retractDiscoveryConfig("select", b == HA_PUMP1 ? "pump1" : b == HA_PUMP2 ? "pump2"
                                        : b == HA_PUMP3   ? "pump3"
                                        : b == HA_PUMP4   ? "pump4"
                                        : b == HA_PUMP5   ? "pump5"
                                                          : "pump6");
      }
    }
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

  // During early boot or after reconnect churn, configuration/info may not be populated yet.
  // Avoid retracting optional retained entities until we have authoritative equipment metadata.
  if (spaConfigurationData.lastUpdate == 0 && spaInformationData.lastUpdate == 0)
  {
    Log.verbose(F("[HA discovery]: skip expanded publish/retract until config/info is available" CR));
    return;
  }

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

  publishWritableEntities(macStr, desired);

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
