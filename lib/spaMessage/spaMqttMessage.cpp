#include "spaMqttMessage.h"
#include "spaMessage.h"
#include "balboa.h"
#include <tempHistory.h>
#include <Arduino.h>
#include <ArduinoLog.h>
#include <mqttModule.h>
#include <spaUtilities.h>
#include <spaCommandDispatcher.h>
#include "spaFilter2Memory.h"

#define BUFFER_SIZE BALBOA_MESSAGE_SIZE * 3 + 1
// Macro to simplify publishing data
#define PUBLISH_STATUS_ELEMENT(element, format)                      \
  do                                                                 \
  {                                                                  \
    char buffer[BUFFER_SIZE];                                        \
    snprintf(buffer, sizeof(buffer), format, spaStatusData.element); \
    publishElement(#element, "status", buffer);                      \
  } while (0)

#define PUBLISH_STATE_MAP(element, map) \
  publishElement(#element, "status", getMapDescription(spaStatusData.element, map).c_str());

void publishElement(const char *element, const char *group, const char *value);

static void publishStatusTemperature(const char *element, float value)
{
  char buffer[BUFFER_SIZE];
  // Balboa reports °F in whole numbers and °C in half-steps.
  // Publish whole numbers for °F and one decimal place for °C.
  const char *format = spaStatusData.tempScale ? "%.1f" : "%.0f";
  snprintf(buffer, sizeof(buffer), format, value);
  publishElement(element, "status", buffer);
}

static void publishStoredRangeSetpoint(const char *element, float value)
{
  if (spaStoredSetpointKnown(value))
  {
    publishStatusTemperature(element, value);
  }
  else
  {
    publishElement(element, "status", "None");
  }
}

void publishElement(const char *element, const char *group, const char *value)
{
  char topic[128];
  snprintf(topic, sizeof(topic), "%s%s/%s", mqttTopic.c_str(), group, element);
  // Log.verbose("Publishing %s: %s\n", topic, value);
  mqtt.publish(topic, value);
}

// Retained variant for state topics that Home Assistant reads after its own restart
// (the controller is not re-queried while the RTC cache is fresh).
static void publishElementRetained(const char *element, const char *group, const char *value)
{
  char topic[128];
  snprintf(topic, sizeof(topic), "%s%s/%s", mqttTopic.c_str(), group, element);
  mqtt.publish(topic, value, true);
}

static void publishFilterTimeRetained(const char *element, uint8_t hour, uint8_t minute)
{
  char buffer[12];
  snprintf(buffer, sizeof(buffer), "%02u:%02u", (unsigned)hour, (unsigned)minute);
  publishElementRetained(element, "filterSettings", buffer);
}

#define convertRawDataToHex(structure, outputBuffer)                             \
  do                                                                             \
  {                                                                              \
    char *ptr = outputBuffer;                                                    \
    for (int i = 0; i < structure.rawDataLength && i < BALBOA_MESSAGE_SIZE; i++) \
    {                                                                            \
      ptr += sprintf(ptr, "%02X ", structure.rawData[i]);                        \
    }                                                                            \
    *(ptr - 1) = '\0';                                                           \
  } while (0)

void publishSpaStatusData()
{
  PUBLISH_STATUS_ELEMENT(crc, "%u");
  PUBLISH_STATUS_ELEMENT(lastUpdate, "%lu");
  PUBLISH_STATUS_ELEMENT(magicNumber, "%u");
  PUBLISH_STATUS_ELEMENT(rawDataLength, "%u");
  // Convert rawData to a space-separated hexadecimal string
  char rawDataHex[3 * BALBOA_MESSAGE_SIZE + 1]; // 2 characters per byte + space + null terminator
  convertRawDataToHex(spaStatusData, rawDataHex);
  publishElement("rawData", "status", rawDataHex);

  publishElement("spaState", "status", spaStateText(spaStatusData.spaState).c_str());
  publishElement("initMode", "status", spaInitModeText(spaStatusData.initMode, spaStatusData.spaState).c_str());
  publishStatusTemperature("currentTemp", spaStatusData.currentTemp);

  publishStatusTemperature("currentTemp", spaStatusData.currentTemp);
  PUBLISH_STATUS_ELEMENT(time, "%s");
  PUBLISH_STATE_MAP(heatingMode, heatingModeMap);
  PUBLISH_STATE_MAP(heatingState, heatingStateMap);

  PUBLISH_STATUS_ELEMENT(reminderType, "%u");
  publishElement("reminderText", "status",
                 spaReminderText(spaStatusData.reminderType, spaStatusData.spaState, spaStatusData.initMode).c_str());
  PUBLISH_STATUS_ELEMENT(sensorA, "%u");
  PUBLISH_STATUS_ELEMENT(sensorB, "%u");
  PUBLISH_STATUS_ELEMENT(tempScale, "%u");
  PUBLISH_STATUS_ELEMENT(clockMode, "%u");
  PUBLISH_STATE_MAP(filterMode, filterModeMap);
  publishElement("filter1_running", "status", spaFilter1Running() ? "On" : "Off");
  publishElement("filter2_running", "status", spaFilter2Running() ? "On" : "Off");

  PUBLISH_STATE_MAP(panelLocked, lockedMap);
  PUBLISH_STATE_MAP(tempRange, tempRangeMap);
  PUBLISH_STATUS_ELEMENT(needsHeat, "%u");

  PUBLISH_STATE_MAP(pump1, pumpMap);
  PUBLISH_STATE_MAP(pump2, pumpMap);
  PUBLISH_STATE_MAP(pump3, pumpMap);
  PUBLISH_STATE_MAP(pump4, pumpMap);
  PUBLISH_STATE_MAP(pump5, pumpMap);
  PUBLISH_STATE_MAP(pump6, pumpMap);
  PUBLISH_STATE_MAP(circ, onOffMap);
  publishElement("blower", "status", spaBlowerBinaryLabel(spaStatusData.blower));
  PUBLISH_STATE_MAP(light1, onOffMap);
  PUBLISH_STATE_MAP(light2, onOffMap);
  PUBLISH_STATE_MAP(mister, onOffMap);

  publishStatusTemperature("setTemp", spaStatusData.setTemp);
  // The inactive range's setpoint may be unknown or from the other scale (#44). "None" makes the
  // HA MQTT sensor report unknown (status topics are not retained, so skipping would leave HA stale).
  publishStoredRangeSetpoint("lowSetTemp", spaStatusData.lowSetTemp);
  publishStoredRangeSetpoint("highSetTemp", spaStatusData.highSetTemp);
  PUBLISH_STATUS_ELEMENT(notification, "%u");
  PUBLISH_STATUS_ELEMENT(flags19, "%u");
  PUBLISH_STATE_MAP(settingsLock, lockedMap);
  PUBLISH_STATUS_ELEMENT(m8CycleTime, "%u");

  PUBLISH_STATUS_ELEMENT(heaterOnTimeToday, "%u");
  PUBLISH_STATUS_ELEMENT(heaterOnTimeYesterday, "%u");
  PUBLISH_STATUS_ELEMENT(filterOnTimeToday, "%u");
  PUBLISH_STATUS_ELEMENT(filterOnTimeYesterday, "%u");

  PUBLISH_STATUS_ELEMENT(filterOnTimeYesterday, "%u");

  publishElement("temperatureHistory", "status", historyToString(const_cast<float *>(tempHistoryData.samples), TEMP_HISTORY_SLOTS).c_str());
  publishElement("heaterHistory", "status", historyToString(spaStatusData.heatOn->history()).c_str());
  publishElement("filterHistory", "status", historyToString(spaStatusData.filterOn->history()).c_str());
}

#define PUBLISH_CONFIG_ELEMENT(element, format)                             \
  do                                                                        \
  {                                                                         \
    char buffer[BUFFER_SIZE];                                               \
    snprintf(buffer, sizeof(buffer), format, spaConfigurationData.element); \
    publishElement(#element, "config", buffer);                             \
  } while (0)

void publishSpaConfigurationData()
{
  PUBLISH_CONFIG_ELEMENT(crc, "%u");
  PUBLISH_CONFIG_ELEMENT(lastUpdate, "%lu");
  PUBLISH_CONFIG_ELEMENT(lastRequest, "%lu");
  PUBLISH_CONFIG_ELEMENT(magicNumber, "%u");
  PUBLISH_CONFIG_ELEMENT(rawDataLength, "%u");

  // Convert rawData to a space-separated hexadecimal string
  char rawDataHex[3 * BALBOA_MESSAGE_SIZE + 1]; // 2 characters per byte + space + null terminator
  convertRawDataToHex(spaConfigurationData, rawDataHex);
  publishElement("rawData", "config", rawDataHex);

  PUBLISH_CONFIG_ELEMENT(pump1, "%u");
  PUBLISH_CONFIG_ELEMENT(pump2, "%u");
  PUBLISH_CONFIG_ELEMENT(pump3, "%u");
  PUBLISH_CONFIG_ELEMENT(pump4, "%u");
  PUBLISH_CONFIG_ELEMENT(pump5, "%u");
  PUBLISH_CONFIG_ELEMENT(pump6, "%u");
  PUBLISH_CONFIG_ELEMENT(light1, "%u");
  PUBLISH_CONFIG_ELEMENT(light2, "%u");
  PUBLISH_CONFIG_ELEMENT(circulationPump, "%u");
  PUBLISH_CONFIG_ELEMENT(blower, "%u");
  PUBLISH_CONFIG_ELEMENT(mister, "%u");
  PUBLISH_CONFIG_ELEMENT(aux1, "%u");
  PUBLISH_CONFIG_ELEMENT(aux2, "%u");
  PUBLISH_CONFIG_ELEMENT(temp_scale, "%u");
}

#define PUBLISH_FILTER_SETTINGS_ELEMENT(element, format)                     \
  do                                                                         \
  {                                                                          \
    char buffer[BUFFER_SIZE];                                                \
    snprintf(buffer, sizeof(buffer), format, spaFilterSettingsData.element); \
    publishElement(#element, "filterSettings", buffer);                      \
  } while (0)

void publishSpaFilterSettingsData()
{
  PUBLISH_FILTER_SETTINGS_ELEMENT(crc, "%u");
  PUBLISH_FILTER_SETTINGS_ELEMENT(lastUpdate, "%lu");
  PUBLISH_FILTER_SETTINGS_ELEMENT(lastRequest, "%lu");
  PUBLISH_FILTER_SETTINGS_ELEMENT(magicNumber, "%u");
  PUBLISH_FILTER_SETTINGS_ELEMENT(rawDataLength, "%u");

  // Convert rawData to a space-separated hexadecimal string
  char rawDataHex[3 * BALBOA_MESSAGE_SIZE + 1]; // 2 characters per byte + space + null terminator
  convertRawDataToHex(spaFilterSettingsData, rawDataHex);
  publishElement("rawData", "filterSettings", rawDataHex);

  PUBLISH_FILTER_SETTINGS_ELEMENT(filt1Hour, "%u");
  PUBLISH_FILTER_SETTINGS_ELEMENT(filt1Minute, "%u");
  PUBLISH_FILTER_SETTINGS_ELEMENT(filt1DurationHour, "%u");
  PUBLISH_FILTER_SETTINGS_ELEMENT(filt1DurationMinute, "%u");
  PUBLISH_FILTER_SETTINGS_ELEMENT(filt2Enable, "%u");
  PUBLISH_FILTER_SETTINGS_ELEMENT(filt2Hour, "%u");
  PUBLISH_FILTER_SETTINGS_ELEMENT(filt2Minute, "%u");
  PUBLISH_FILTER_SETTINGS_ELEMENT(filt2DurationHour, "%u");
  PUBLISH_FILTER_SETTINGS_ELEMENT(filt2DurationMinute, "%u");

  // Combined HH:MM / on-off state topics (retained). They use the same format as the
  // cmd/filter/filter{1,2}/{start,duration,enabled} write topics, so Home Assistant text/switch
  // entities can read and write the schedule with one topic pair each.
  publishFilterTimeRetained("filter1_start", spaFilterSettingsData.filt1Hour, spaFilterSettingsData.filt1Minute);
  publishFilterTimeRetained("filter1_duration", spaFilterSettingsData.filt1DurationHour, spaFilterSettingsData.filt1DurationMinute);
  publishElementRetained("filter2_enabled", "filterSettings", spaFilterSettingsData.filt2Enable ? "on" : "off");
  // While filter 2 is off the controller reports zeros; show the remembered schedule instead
  // (what switching filter 2 on will use), so the entities stay editable.
  SpaFilter2Schedule f2 = {spaFilterSettingsData.filt2Hour, spaFilterSettingsData.filt2Minute,
                           spaFilterSettingsData.filt2DurationHour, spaFilterSettingsData.filt2DurationMinute};
  if (!spaFilterSettingsData.filt2Enable)
  {
    f2 = spaFilter2MemoryGet(spaFilterSettingsData.filt1Hour, spaFilterSettingsData.filt1Minute,
                             spaFilterSettingsData.filt1DurationHour, spaFilterSettingsData.filt1DurationMinute);
  }
  publishFilterTimeRetained("filter2_start", f2.startHour, f2.startMinute);
  publishFilterTimeRetained("filter2_duration", f2.durHour, f2.durMinute);
}

#define PUBLISH_FAULT_LOG_ELEMENT(element, format)                     \
  do                                                                   \
  {                                                                    \
    char buffer[64];                                                   \
    snprintf(buffer, sizeof(buffer), format, spaFaultLogData.element); \
    publishElement(#element, "faultLog", buffer);                      \
  } while (0)

void publishSpaFaultLogData()
{
  PUBLISH_FAULT_LOG_ELEMENT(crc, "%u");
  PUBLISH_FAULT_LOG_ELEMENT(lastUpdate, "%lu");
  PUBLISH_FAULT_LOG_ELEMENT(lastRequest, "%lu");
  PUBLISH_FAULT_LOG_ELEMENT(magicNumber, "%u");

  PUBLISH_FAULT_LOG_ELEMENT(rawDataLength, "%u");
  // Convert rawData to a space-separated hexadecimal string
  char rawDataHex[3 * BALBOA_MESSAGE_SIZE + 1]; // 2 characters per byte + space + null terminator
  convertRawDataToHex(spaFaultLogData, rawDataHex);
  publishElement("rawData", "faultLog", rawDataHex);

  PUBLISH_FAULT_LOG_ELEMENT(totEntry, "%u");
  PUBLISH_FAULT_LOG_ELEMENT(currEntry, "%u");
  PUBLISH_FAULT_LOG_ELEMENT(faultCode, "%u");

  // Handle publishing of String type
  publishElement("faultMessage", "faultLog", spaFaultLogData.faultMessage.c_str());

  PUBLISH_FAULT_LOG_ELEMENT(daysAgo, "%u");
  PUBLISH_FAULT_LOG_ELEMENT(hour, "%u");
  PUBLISH_FAULT_LOG_ELEMENT(minutes, "%u");
  publishElement("faultLogTime", "faultLog", spaFormatFaultLogTime(spaFaultLogData).c_str());
}

#define PUBLISH_PREFERENCES_ELEMENT(element, format)                      \
  do                                                                      \
  {                                                                       \
    char buffer[BUFFER_SIZE];                                             \
    snprintf(buffer, sizeof(buffer), format, spaPreferencesData.element); \
    publishElement(#element, "preferences", buffer);                      \
  } while (0)

void publishSpaPreferencesData()
{
  PUBLISH_PREFERENCES_ELEMENT(crc, "%u");
  PUBLISH_PREFERENCES_ELEMENT(lastUpdate, "%lu");
  PUBLISH_PREFERENCES_ELEMENT(lastRequest, "%lu");
  PUBLISH_PREFERENCES_ELEMENT(magicNumber, "%u");

  PUBLISH_PREFERENCES_ELEMENT(rawDataLength, "%u");
  // Convert rawData to a space-separated hexadecimal string
  char rawDataHex[3 * BALBOA_MESSAGE_SIZE + 1]; // 2 characters per byte + space + null terminator
  convertRawDataToHex(spaPreferencesData, rawDataHex);
  publishElement("rawData", "preferences", rawDataHex);

  PUBLISH_PREFERENCES_ELEMENT(reminders, "%u");
  PUBLISH_PREFERENCES_ELEMENT(tempScale, "%u");
  PUBLISH_PREFERENCES_ELEMENT(clockMode, "%u");
  PUBLISH_PREFERENCES_ELEMENT(cleanupCycle, "%u");
  PUBLISH_PREFERENCES_ELEMENT(dolphinAddress, "%u");
  PUBLISH_PREFERENCES_ELEMENT(m8AI, "%u");
}

#define PUBLISH_INFORMATION_ELEMENT(element, format)                      \
  do                                                                      \
  {                                                                       \
    char buffer[BUFFER_SIZE];                                             \
    snprintf(buffer, sizeof(buffer), format, spaInformationData.element); \
    publishElement(#element, "information", buffer);                      \
  } while (0)

void publishSpaInformationData()
{
  PUBLISH_INFORMATION_ELEMENT(crc, "%u");
  PUBLISH_INFORMATION_ELEMENT(lastUpdate, "%lu");
  PUBLISH_INFORMATION_ELEMENT(lastRequest, "%lu");
  PUBLISH_INFORMATION_ELEMENT(magicNumber, "%u");

  PUBLISH_INFORMATION_ELEMENT(rawDataLength, "%u");
  // Convert rawData to a space-separated hexadecimal string
  char rawDataHex[3 * BALBOA_MESSAGE_SIZE + 1]; // 2 characters per byte + space + null terminator
  convertRawDataToHex(spaInformationData, rawDataHex);
  publishElement("rawData", "information", rawDataHex);

  // Handle publishing of char arrays
  publishElement("softwareID", "information", spaInformationData.softwareID);
  publishElement("model", "information", spaInformationData.model);
  PUBLISH_INFORMATION_ELEMENT(setupNumber, "%u");
  PUBLISH_INFORMATION_ELEMENT(voltage, "%u");
  PUBLISH_INFORMATION_ELEMENT(heaterType, "%u");
  publishElement("dipSwitch", "information", spaInformationData.dipSwitch);
}

#define PUBLISH_SETTINGS_0x04_ELEMENT(element, format)                     \
  do                                                                       \
  {                                                                        \
    char buffer[BUFFER_SIZE];                                              \
    snprintf(buffer, sizeof(buffer), format, spaSettings0x04Data.element); \
    publishElement(#element, "settings0x04", buffer);                      \
  } while (0)

void publishSpaSettings0x04Data()
{
  PUBLISH_SETTINGS_0x04_ELEMENT(crc, "%u");
  PUBLISH_SETTINGS_0x04_ELEMENT(lastUpdate, "%lu");
  PUBLISH_SETTINGS_0x04_ELEMENT(lastRequest, "%lu");
  PUBLISH_SETTINGS_0x04_ELEMENT(magicNumber, "%u");

  PUBLISH_SETTINGS_0x04_ELEMENT(rawDataLength, "%u");
  // Convert rawData to a space-separated hexadecimal string
  char rawDataHex[3 * BALBOA_MESSAGE_SIZE + 1]; // 2 characters per byte + space + null terminator
  convertRawDataToHex(spaSettings0x04Data, rawDataHex);
  publishElement("rawData", "settings0x04", rawDataHex);
}

#define PUBLISH_WIFI_MODULE_CONFIG_ELEMENT(element, format)                        \
  do                                                                               \
  {                                                                                \
    char buffer[BUFFER_SIZE];                                                      \
    snprintf(buffer, sizeof(buffer), format, wiFiModuleConfigurationData.element); \
    publishElement(#element, "wifiModuleConfig", buffer);                          \
  } while (0)

void publishWiFiModuleConfigurationData()
{
  PUBLISH_WIFI_MODULE_CONFIG_ELEMENT(crc, "%u");
  PUBLISH_WIFI_MODULE_CONFIG_ELEMENT(lastUpdate, "%lu");
  PUBLISH_WIFI_MODULE_CONFIG_ELEMENT(lastRequest, "%lu");
  PUBLISH_WIFI_MODULE_CONFIG_ELEMENT(magicNumber, "%u");

  // Handle publishing of char arrays
  publishElement("macAddress", "wifiModuleConfig", wiFiModuleConfigurationData.macAddress);
}
