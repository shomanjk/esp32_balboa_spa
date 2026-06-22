#include <Arduino.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <TickTwo.h>
#include <ArduinoLog.h>
#include <ctype.h>
#include <string.h>

// Local Libraries

#include <wifiModule.h>
#include <spaUtilities.h>
#include <restartReason.h>
#include "mqttModule.h"
#include "haMqttDiscovery.h"
#include <rs485.h>
#include <spaCommandDispatcher.h>

// Local Functions
void reconnect();
void mqttMessage(char *p_topic, byte *p_payload, unsigned int p_length);
void nodeStateReport();
void publishCommandResult(const char *target, const String &value, const SpaCommandResult &result);
bool parseClockPayload(const String &payload, uint8_t &hour24, uint8_t &minute);
String gatewayClockPayload();

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
String mqttTopic = "Spa/"; // root topic, gets appeanded with node mac address
unsigned long mqttLastReconnectAttempt = 0;

TickTwo sendStatus(nodeStateReport, 1.5 * 60 * 1000); // 5 minutes

#define MQTT_RECONNECT_INTERVAL_MS 30000

namespace
{
bool equalsIgnoreCaseTrimmed(const String &value, const char *needle)
{
  String x = value;
  x.trim();
  return x.equalsIgnoreCase(needle);
}

int parseIntStrict(const String &value)
{
  String t = value;
  t.trim();
  if (t.length() == 0)
  {
    return -1;
  }
  for (int i = 0; i < t.length(); i++)
  {
    if (i == 0 && (t[i] == '-' || t[i] == '+'))
    {
      continue;
    }
    if (t[i] < '0' || t[i] > '9')
    {
      return -1;
    }
  }
  return t.toInt();
}
} // namespace

void mqttModuleSetup()
{
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(mqttMessage);
  mqtt.setKeepAlive(60);
  mqtt.setSocketTimeout(5);
  mqtt.setBufferSize(4096); // discovery JSON + state publishes
  mqttTopic = mqttTopic + String(gatewayName) + "/";
  Log.notice("MQTT Server: %s:%d\n", MQTT_SERVER, MQTT_PORT);
  Log.notice("MQTT Topic: %s\n", mqttTopic.c_str());
  sendStatus.start();
}

void mqttModuleLoop()
{
  if (!mqtt.connected())
  {
    if (mqttLastReconnectAttempt == 0 || millis() - mqttLastReconnectAttempt >= MQTT_RECONNECT_INTERVAL_MS)
    {
      mqttLastReconnectAttempt = millis();
      reconnect();
    }
    return;
  }
  sendStatus.update();
  mqtt.loop();
}

void reconnect()
{
  // int oldstate = mqtt.state();
  // boolean connection = false;
  //  Loop until we're reconnected
  if (!mqtt.connected())
  {
    // Attempt to connect

    mqtt.connect(gatewayName, BROKER_LOGIN, BROKER_PASS, (mqttTopic + "node/state").c_str(), 1, true, "OFF");

    if (mqtt.connected())
    {
      publishError("MQTT Timeout - Reconnect Successfully Run");
      mqtt.subscribe((mqttTopic + "cmd/#").c_str());
      nodeStateReport();
#if MQTT_HA_DISCOVERY
      publishHomeAssistantDiscovery();
#endif
    }
  }
}

void mqttMessage(char *p_topic, byte *p_payload, unsigned int p_length)
{
  String topic = String(p_topic);
  const String cmdPrefix = mqttTopic + "cmd/";
  if (!topic.startsWith(cmdPrefix))
  {
    return;
  }

  String payload = "";
  for (unsigned int i = 0; i < p_length; i++)
  {
    payload += (char)p_payload[i];
  }
  payload.trim();

  String suffix = topic.substring(cmdPrefix.length());
  SpaCommandResult result = {false, SPA_COMMAND_INVALID_ARGUMENT, "unsupported_command"};
  String target = suffix;

  if (suffix == "setTemp")
  {
    if (payload.length() == 0)
    {
      result = {false, SPA_COMMAND_INVALID_ARGUMENT, "invalid_temp_payload"};
    }
    else
    {
      result = spaSetTargetTemperature(payload.toFloat(), SPA_COMMAND_SOURCE_MQTT);
    }
  }
  else if (suffix == "setTime")
  {
    uint8_t hour24 = 0;
    uint8_t minute = 0;
    if (!parseClockPayload(payload, hour24, minute))
    {
      result = {false, SPA_COMMAND_INVALID_ARGUMENT, "invalid_time_payload"};
    }
    else
    {
      result = spaSetSpaPanelClockTime(hour24, minute, SPA_COMMAND_SOURCE_MQTT);
    }
  }
  else if (suffix == "syncTime")
  {
    if (payload.length() == 0)
    {
      result = {false, SPA_COMMAND_INVALID_ARGUMENT, "invalid_sync_payload"};
    }
    else
    {
      String hhmm = gatewayClockPayload();
      uint8_t hour24 = 0;
      uint8_t minute = 0;
      if (!parseClockPayload(hhmm, hour24, minute))
      {
        result = {false, SPA_COMMAND_INVALID_ARGUMENT, "gateway_clock_unavailable"};
      }
      else
      {
        payload = hhmm;
        result = spaSetSpaPanelClockTime(hour24, minute, SPA_COMMAND_SOURCE_MQTT);
      }
    }
  }
  else if (suffix == "mode")
  {
    if (equalsIgnoreCaseTrimmed(payload, "heat"))
    {
      result = spaSetHeatingMode(true, SPA_COMMAND_SOURCE_MQTT);
    }
    else if (equalsIgnoreCaseTrimmed(payload, "off"))
    {
      result = spaSetHeatingMode(false, SPA_COMMAND_SOURCE_MQTT);
    }
    else
    {
      result = {false, SPA_COMMAND_INVALID_ARGUMENT, "invalid_mode_payload"};
    }
  }
  else if (suffix == "preset")
  {
    if (equalsIgnoreCaseTrimmed(payload, "high range"))
    {
      result = spaSetTempRange(true, SPA_COMMAND_SOURCE_MQTT);
    }
    else if (equalsIgnoreCaseTrimmed(payload, "low range"))
    {
      result = spaSetTempRange(false, SPA_COMMAND_SOURCE_MQTT);
    }
    else
    {
      result = {false, SPA_COMMAND_INVALID_ARGUMENT, "invalid_preset_payload"};
    }
  }
  else if (suffix == "tempUnits")
  {
    if (equalsIgnoreCaseTrimmed(payload, "c") || equalsIgnoreCaseTrimmed(payload, "celsius"))
    {
      result = spaSetTemperatureScale(true, SPA_COMMAND_SOURCE_MQTT);
    }
    else if (equalsIgnoreCaseTrimmed(payload, "f") || equalsIgnoreCaseTrimmed(payload, "fahrenheit"))
    {
      result = spaSetTemperatureScale(false, SPA_COMMAND_SOURCE_MQTT);
    }
    else
    {
      result = {false, SPA_COMMAND_INVALID_ARGUMENT, "invalid_temp_units_payload"};
    }
  }
  else if (suffix.startsWith("button/"))
  {
    String codeRaw = suffix.substring(7);
    int itemCode = parseIntStrict(codeRaw);
    if (itemCode <= 0 || itemCode > 255)
    {
      result = {false, SPA_COMMAND_INVALID_ARGUMENT, "invalid_button_payload"};
    }
    else if (itemCode >= 4 && itemCode <= 9)
    {
      uint8_t desiredSpeed = 0xFF;
      if (equalsIgnoreCaseTrimmed(payload, "off"))
      {
        desiredSpeed = 0;
      }
      else if (equalsIgnoreCaseTrimmed(payload, "low") || equalsIgnoreCaseTrimmed(payload, "on"))
      {
        desiredSpeed = 1;
      }
      else if (equalsIgnoreCaseTrimmed(payload, "high"))
      {
        desiredSpeed = 2;
      }
      if (desiredSpeed > 2)
      {
        result = {false, SPA_COMMAND_INVALID_ARGUMENT, "invalid_pump_payload"};
      }
      else
      {
        result = spaSendButtonForPumpSpeed((uint8_t)(itemCode - 3), desiredSpeed, SPA_COMMAND_SOURCE_MQTT);
      }
    }
    else
    {
      bool desiredOn = false;
      bool hasDesired = true;
      if (equalsIgnoreCaseTrimmed(payload, "on"))
      {
        desiredOn = true;
      }
      else if (equalsIgnoreCaseTrimmed(payload, "off"))
      {
        desiredOn = false;
      }
      else if (equalsIgnoreCaseTrimmed(payload, "toggle"))
      {
        SpaCommandResult toggleResult = spaSendToggleCommand((uint8_t)itemCode, SPA_COMMAND_SOURCE_MQTT);
        publishCommandResult(suffix.c_str(), payload, toggleResult);
        return;
      }
      else
      {
        hasDesired = false;
      }

      if (!hasDesired)
      {
        result = {false, SPA_COMMAND_INVALID_ARGUMENT, "invalid_button_payload"};
      }
      else
      {
        result = spaSendButtonForBinaryState((uint8_t)itemCode, desiredOn, SPA_COMMAND_SOURCE_MQTT);
      }
    }
  }
  else if (suffix == "filter")
  {
    if (payload.length() == 0)
    {
      result = {false, SPA_COMMAND_INVALID_ARGUMENT, "invalid_filter_payload"};
    }
    else
    {
      DynamicJsonDocument doc(512);
      const DeserializationError jsonErr = deserializeJson(doc, payload);
      if (jsonErr)
      {
        result = {false, SPA_COMMAND_INVALID_ARGUMENT, "bad_json"};
      }
      else
      {
        SpaFilterCycleSettings settings{};
        const char *err = nullptr;
        if (!spaParseFilterCycleJson(doc.as<JsonObjectConst>(), settings, true, &err))
        {
          result = {false, SPA_COMMAND_INVALID_ARGUMENT, err ? err : "invalid_filter_payload"};
        }
        else
        {
          result = spaSetFilterCycles(settings, SPA_COMMAND_SOURCE_MQTT);
        }
      }
    }
  }
  else if (suffix.startsWith("filter/"))
  {
    SpaFilterCycleSettings settings{};
    const char *err = nullptr;
    if (!spaApplyFilterGranularMqtt(suffix, payload, settings, &err))
    {
      if (err && strcmp(err, "unsupported_command") == 0)
      {
        result = {false, SPA_COMMAND_INVALID_ARGUMENT, "unsupported_command"};
      }
      else
      {
        result = {false, SPA_COMMAND_INVALID_ARGUMENT, err ? err : "invalid_filter_payload"};
      }
    }
    else
    {
      result = spaSetFilterCycles(settings, SPA_COMMAND_SOURCE_MQTT);
    }
  }

  publishCommandResult(target.c_str(), payload, result);
}

void publishCommandResult(const char *target, const String &value, const SpaCommandResult &result)
{
  StaticJsonDocument<256> doc;
  doc["target"] = target;
  doc["value"] = value;
  doc["accepted"] = result.accepted;
  doc["reason"] = result.reason;
  char payload[256];
  size_t n = serializeJson(doc, payload, sizeof(payload));
  if (n == 0 || n >= sizeof(payload))
  {
    return;
  }
  mqtt.publish((mqttTopic + "cmd/result").c_str(), (const uint8_t *)payload, (unsigned int)n, false);
}

bool parseClockPayload(const String &payload, uint8_t &hour24, uint8_t &minute)
{
  if (payload.length() != 5 || payload[2] != ':')
  {
    return false;
  }
  if (!isdigit((unsigned char)payload[0]) || !isdigit((unsigned char)payload[1]) ||
      !isdigit((unsigned char)payload[3]) || !isdigit((unsigned char)payload[4]))
  {
    return false;
  }

  int hour = (payload[0] - '0') * 10 + (payload[1] - '0');
  int min = (payload[3] - '0') * 10 + (payload[4] - '0');
  if (hour < 0 || hour > 23 || min < 0 || min > 59)
  {
    return false;
  }
  hour24 = (uint8_t)hour;
  minute = (uint8_t)min;
  return true;
}

String gatewayClockPayload()
{
  time_t t = getTime();
  if (t <= 0)
  {
    return String("");
  }
  struct tm tmStore;
  struct tm *p = localtime_r(&t, &tmStore);
  if (p == nullptr)
  {
    return String("");
  }
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", p->tm_hour, p->tm_min);
  return String(buf);
}

void nodeStateReport()
{
  if (mqtt.connected())
  {
    publishNodeStatus("ip", WiFi.localIP().toString().c_str());
    publishNodeStatus("mac", WiFi.macAddress().c_str());
    publishNodeStatus("gateway", gatewayName);
    publishNodeStatus("restartReason", getLastRestartReason().c_str());
    publishNodeStatus("uptime", String(millis() / 1000).c_str());
    publishNodeStatus("getTime", String(getTime()).c_str());
    publishNodeStatus("state", "ON", true);
    publishNodeStatus("flashsize", String(ESP.getFlashChipSize()).c_str());
    publishNodeStatus("chipid", String(ESP.getChipModel()).c_str());
    publishNodeStatus("speed", String(ESP.getCpuFreqMHz()).c_str());
    publishNodeStatus("heap", String(ESP.getFreeHeap()).c_str());
    publishNodeStatus("psram", String(ESP.getFreePsram()).c_str());
    publishNodeStatus("stack", String(uxTaskGetStackHighWaterMark(NULL)).c_str());
#ifdef LOCAL_CLIENT
    publishNodeStatus("rs485 messagesToday", String(rs485Stats.messagesToday).c_str());
    publishNodeStatus("rs485 crcToday", String(rs485Stats.crcToday).c_str());
    publishNodeStatus("rs485 messagesYesterday", String(rs485Stats.messagesYesterday).c_str());
    publishNodeStatus("rs485 crcYesterday", String(rs485Stats.crcYesterday).c_str());
    publishNodeStatus("rs485 badFormatToday", String(rs485Stats.badFormatToday).c_str());
    publishNodeStatus("rs485 badFormatYesterday", String(rs485Stats.badFormatYesterday).c_str());
#endif

    String release = String(__DATE__) + " - " + String(__TIME__);
    publishNodeStatus("release", release.c_str());
    publishNodeStatus("buildDefinition", buildDefinitionString.c_str());
  }
}
