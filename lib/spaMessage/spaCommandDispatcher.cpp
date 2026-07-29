#include "spaCommandDispatcher.h"

#include <ArduinoLog.h>
#include <CircularBuffer.hpp>
#include <WiFi.h>
#include <math.h>

#include "spaMessage.h"
#include "balboa.h"
#include "rs485.h"
#include <spaUtilities.h>

namespace
{
const uint8_t kBroadcastChannel = 0xBF;

const char *sourceLabel(SpaCommandSource source)
{
  switch (source)
  {
  case SPA_COMMAND_SOURCE_WEB:
    return "web";
  case SPA_COMMAND_SOURCE_MQTT:
    return "mqtt";
  case SPA_COMMAND_SOURCE_AUTO:
    return "auto";
  default:
    return "unknown";
  }
}

uint8_t destinationId()
{
  return WIFI_MODULE_ID;
}

uint8_t destinationIdForMode(bool useWifiDestination)
{
  if (useWifiDestination)
  {
    return WIFI_MODULE_ID;
  }
  return id;
}

SpaCommandResult queueFrame(CircularBuffer<uint8_t, BALBOA_MESSAGE_SIZE> &frame, const char *logAction, SpaCommandSource source, String *outFrameHex = nullptr)
{
  addCRC(frame);
  if (outFrameHex != nullptr)
  {
    *outFrameHex = msgToString(frame);
  }
  if (!sendMessageToSpa(frame))
  {
#ifdef LOCAL_CLIENT
    if (!rs485UartBegun())
    {
      Log.warning(F("[Cmd ]: rejected %s from %s (UART not ready): %s" CR),
                  logAction, sourceLabel(source), msgToString(frame).c_str());
      return {false, SPA_COMMAND_NOT_READY, "rs485_uart_not_ready"};
    }
#endif
    Log.warning(F("[Cmd ]: rejected %s from %s (write queue full): %s" CR),
                logAction, sourceLabel(source), msgToString(frame).c_str());
    return {false, SPA_COMMAND_NOT_READY, "spa_write_queue_full"};
  }
  Log.verbose(F("[Cmd ]: accepted %s from %s: %s" CR), logAction, sourceLabel(source), msgToString(frame).c_str());
  return {true, SPA_COMMAND_ACCEPTED, "accepted"};
}
} // namespace

void spaProtocolActiveSetpointBand(float &minBand, float &maxBand)
{
  const bool celsius = spaStatusData.tempScale != 0;
  const bool highRange = spaStatusData.tempRange != 0;
  if (!celsius)
  {
    if (highRange)
    {
      minBand = 80.0f;
      maxBand = 104.0f;
    }
    else
    {
      minBand = 50.0f;
      maxBand = 80.0f;
    }
  }
  else
  {
    if (highRange)
    {
      minBand = 26.0f;
      maxBand = 40.0f;
    }
    else
    {
      minBand = 10.0f;
      maxBand = 26.0f;
    }
  }
}

bool spaHasFreshStatus()
{
  return !staleData(spaStatusData);
}

SpaCommandResult spaSendToggleCommand(uint8_t itemCode, SpaCommandSource source)
{
  // Toggle requests can be sent once status has been seen; they do not require
  // configuration frames to be present.
  if (!spaHasFreshStatus())
  {
    return {false, SPA_COMMAND_NOT_READY, "spa status not ready"};
  }

  CircularBuffer<uint8_t, BALBOA_MESSAGE_SIZE> frame;
  frame.push(destinationId());
  frame.push(kBroadcastChannel);
  frame.push(Toggle_Item_Request_Type);
  frame.push(itemCode);
  frame.push(0x00);
  return queueFrame(frame, "toggle", source);
}

int spaPumpToggleCountForSpeed(uint8_t pumpId, uint8_t desiredSpeed)
{
  if (pumpId < 1 || pumpId > 6 || desiredSpeed > 2)
  {
    return -1;
  }
  const uint8_t pumpStatus[] = {spaStatusData.pump1, spaStatusData.pump2, spaStatusData.pump3, spaStatusData.pump4, spaStatusData.pump5, spaStatusData.pump6};
  const uint8_t pumpConfig[] = {spaConfigurationData.pump1, spaConfigurationData.pump2, spaConfigurationData.pump3, spaConfigurationData.pump4, spaConfigurationData.pump5, spaConfigurationData.pump6};
  const uint8_t state = pumpStatus[pumpId - 1];
  const uint8_t speedConfig = pumpConfig[pumpId - 1];
  if (speedConfig == 0)
  {
    return -1;
  }
  if (speedConfig <= 1)
  {
    // Single-speed pumps map 0->Off and 1->On. "High" is invalid for these pumps.
    if (desiredSpeed == 2)
    {
      return -1;
    }
    const uint8_t desiredOnOff = desiredSpeed > 0 ? 1 : 0;
    const uint8_t currentOnOff = state > 0 ? 1 : 0;
    return (desiredOnOff == currentOnOff) ? 0 : 1;
  }

  if (state > 2)
  {
    return -1;
  }
  const int delta = ((int)desiredSpeed - (int)state + 3) % 3;
  return delta;
}

int spaToggleCountForButtonRequest(uint8_t itemCode, bool requestHasState, bool desiredOn)
{
  // Lights are binary; one toggle transitions state.
  if (itemCode == 17 || itemCode == 18)
  {
    if (!requestHasState)
    {
      return 1;
    }
    const bool isOn = (itemCode == 17 ? spaStatusData.light1 : spaStatusData.light2);
    return (isOn == desiredOn) ? 0 : 1;
  }

  // Pumps can be single-speed or two-speed.
  if (itemCode >= 4 && itemCode <= 9)
  {
    if (!requestHasState)
    {
      return 1;
    }
    const uint8_t pumpId = (itemCode - 3);
    const uint8_t desiredSpeed = desiredOn ? 1 : 0;
    return spaPumpToggleCountForSpeed(pumpId, desiredSpeed);
  }

  // Balboa temp range: item 0x50 (80) toggles high/low.
  if (itemCode == 80)
  {
    if (!requestHasState)
    {
      return 1;
    }
    const bool isHigh = (spaStatusData.tempRange != 0);
    const bool wantHigh = desiredOn;
    return (isHigh == wantHigh) ? 0 : 1;
  }

  // Heating mode item 0x51 (81) toggles Ready/Rest.
  if (itemCode == 81)
  {
    if (!requestHasState)
    {
      return 1;
    }
    const bool inRest = (spaStatusData.heatingMode == 1);
    const bool wantReady = desiredOn;
    return (wantReady == !inRest) ? 0 : 1;
  }

  // Other items remain single-toggle for now.
  return 1;
}

SpaCommandResult spaSendButtonForBinaryState(uint8_t itemCode, bool desiredOn, SpaCommandSource source)
{
  const int togglesToSend = spaToggleCountForButtonRequest(itemCode, true, desiredOn);
  if (togglesToSend < 0)
  {
    return {false, SPA_COMMAND_INVALID_ARGUMENT, "invalid button request"};
  }
  if (togglesToSend == 0)
  {
    return {true, SPA_COMMAND_ACCEPTED, "accepted"};
  }

  SpaCommandResult result = {false, SPA_COMMAND_INVALID_ARGUMENT, "unknown"};
  for (int i = 0; i < togglesToSend; i++)
  {
    result = spaSendToggleCommand(itemCode, source);
    if (!result.accepted)
    {
      return result;
    }
  }
  return result;
}

SpaCommandResult spaSendButtonForPumpSpeed(uint8_t pumpId, uint8_t desiredSpeed, SpaCommandSource source)
{
  if (pumpId < 1 || pumpId > 6)
  {
    return {false, SPA_COMMAND_INVALID_ARGUMENT, "invalid pump id"};
  }
  const int togglesToSend = spaPumpToggleCountForSpeed(pumpId, desiredSpeed);
  if (togglesToSend < 0)
  {
    return {false, SPA_COMMAND_INVALID_ARGUMENT, "invalid pump speed"};
  }
  if (togglesToSend == 0)
  {
    return {true, SPA_COMMAND_ACCEPTED, "accepted"};
  }
  const uint8_t itemCode = (uint8_t)(pumpId + 3);
  SpaCommandResult result = {false, SPA_COMMAND_INVALID_ARGUMENT, "unknown"};
  for (int i = 0; i < togglesToSend; i++)
  {
    result = spaSendToggleCommand(itemCode, source);
    if (!result.accepted)
    {
      return result;
    }
  }
  return result;
}

SpaCommandResult spaSetHeatingMode(bool ready, SpaCommandSource source)
{
  return spaSendButtonForBinaryState(81, ready, source);
}

SpaCommandResult spaSetTempRange(bool high, SpaCommandSource source)
{
  return spaSendButtonForBinaryState(80, high, source);
}

SpaCommandResult spaSetTargetTemperature(float targetTemperature, SpaCommandSource source)
{
  if (!spaHasFreshStatus())
  {
    return {false, SPA_COMMAND_NOT_READY, "spa status not ready"};
  }

  float minBand = 0;
  float maxBand = 0;
  spaProtocolActiveSetpointBand(minBand, maxBand);
  const float tol = spaStatusData.tempScale ? 0.26f : 0.01f;
  if (targetTemperature < minBand - tol || targetTemperature > maxBand + tol)
  {
    return {false, SPA_COMMAND_INVALID_ARGUMENT, "setpoint outside protocol range"};
  }

  // Balboa setpoint encoding uses 0.5-degree units for C mode, 1-degree units for F mode.
  const float rawScaled = (spaStatusData.tempScale ? targetTemperature * 2.0f : targetTemperature);
  const int encoded = (int)roundf(rawScaled);
  if (encoded < 0 || encoded > 255)
  {
    return {false, SPA_COMMAND_INVALID_ARGUMENT, "target encode overflow"};
  }

  CircularBuffer<uint8_t, BALBOA_MESSAGE_SIZE> frame;
  frame.push(destinationId());
  frame.push(kBroadcastChannel);
  frame.push(Set_Temperature_Type);
  frame.push((uint8_t)encoded);
  return queueFrame(frame, "set_temp", source);
}

SpaCommandResult spaSetTemperatureScale(bool celsius, SpaCommandSource source)
{
  if (!spaHasFreshStatus())
  {
    return {false, SPA_COMMAND_NOT_READY, "spa status not ready"};
  }

  CircularBuffer<uint8_t, BALBOA_MESSAGE_SIZE> frame;
  frame.push(destinationId());
  frame.push(kBroadcastChannel);
  frame.push(Set_Preference_Request_Type);
  frame.push(0x01);
  frame.push(celsius ? 0x01 : 0x00);
  return queueFrame(frame, "set_temp_scale", source);
}

SpaCommandResult spaSetPanelReminders(bool enabled, SpaCommandSource source)
{
  if (!spaHasFreshStatus())
  {
    return {false, SPA_COMMAND_NOT_READY, "spa status not ready"};
  }

  CircularBuffer<uint8_t, BALBOA_MESSAGE_SIZE> frame;
  frame.push(destinationId());
  frame.push(kBroadcastChannel);
  frame.push(Set_Preference_Request_Type);
  frame.push(0x00);
  frame.push(enabled ? 0x01 : 0x00);
  return queueFrame(frame, "set_reminders", source);
}

SpaCommandResult spaSetSpaPanelClockFormat(bool use24Hour, SpaCommandSource source)
{
  if (!spaHasFreshStatus())
  {
    return {false, SPA_COMMAND_NOT_READY, "spa status not ready"};
  }

  // Status payload carries panel clock as "HH:MM" (hour remains 0-23 regardless
  // of panel display mode); TimeFormat write piggybacks Set Time and changes bit 7.
  const char *t = spaStatusData.time;
  if (t == nullptr || strlen(t) < 5 || t[2] != ':')
  {
    return {false, SPA_COMMAND_INVALID_ARGUMENT, "panel_time_unavailable"};
  }
  if (t[0] < '0' || t[0] > '9' || t[1] < '0' || t[1] > '9' ||
      t[3] < '0' || t[3] > '9' || t[4] < '0' || t[4] > '9')
  {
    return {false, SPA_COMMAND_INVALID_ARGUMENT, "panel_time_unavailable"};
  }
  const uint8_t hour24 = (uint8_t)((t[0] - '0') * 10 + (t[1] - '0'));
  const uint8_t minute = (uint8_t)((t[3] - '0') * 10 + (t[4] - '0'));
  if (hour24 > 23 || minute > 59)
  {
    return {false, SPA_COMMAND_INVALID_ARGUMENT, "panel_time_unavailable"};
  }

  uint8_t hourByte = hour24 & 0x7F;
  if (use24Hour)
  {
    hourByte |= 0x80;
  }

  CircularBuffer<uint8_t, BALBOA_MESSAGE_SIZE> frame;
  frame.push(destinationId());
  frame.push(kBroadcastChannel);
  frame.push(Set_Time_Type);
  frame.push(hourByte);
  frame.push(minute);
  return queueFrame(frame, "set_time_format", source);
}

namespace
{
bool filterDurationValid(uint8_t durHour, uint8_t durMinute, bool allowZero)
{
  if (durHour > 24 || durMinute > 59)
  {
    return false;
  }
  if (durHour == 24 && durMinute != 0)
  {
    return false;
  }
  const bool zeroDuration = durHour == 0 && durMinute == 0;
  if (!allowZero && zeroDuration)
  {
    return false;
  }
  return true;
}

bool filterStartValid(uint8_t hour, uint8_t minute)
{
  return hour <= 23 && minute <= 59;
}
} // namespace

void spaNormalizeFilter2Schedule(SpaFilterCycleSettings &settings)
{
  if (!settings.filt2Enable)
  {
    settings.filt2Hour = 0;
    settings.filt2Minute = 0;
    settings.filt2DurHour = 0;
    settings.filt2DurMinute = 0;
  }
}

void spaNormalizeFilter2Cache(SpaFilterSettingsData &data)
{
  if (!data.filt2Enable)
  {
    data.filt2Hour = 0;
    data.filt2Minute = 0;
    data.filt2DurationHour = 0;
    data.filt2DurationMinute = 0;
  }
}

bool spaValidateFilterCycleSettings(const SpaFilterCycleSettings &settings, const char **outReason)
{
  if (!filterStartValid(settings.filt1Hour, settings.filt1Minute))
  {
    if (outReason)
    {
      *outReason = "invalid_filter1_start";
    }
    return false;
  }
  if (!filterDurationValid(settings.filt1DurHour, settings.filt1DurMinute, false))
  {
    if (outReason)
    {
      *outReason = "invalid_filter1_duration";
    }
    return false;
  }
  if (settings.filt2Enable)
  {
    if (!filterStartValid(settings.filt2Hour, settings.filt2Minute))
    {
      if (outReason)
      {
        *outReason = "invalid_filter2_start";
      }
      return false;
    }
    if (!filterDurationValid(settings.filt2DurHour, settings.filt2DurMinute, false))
    {
      if (outReason)
      {
        *outReason = "invalid_filter2_duration";
      }
      return false;
    }
  }
  return true;
}

bool spaParseFilterCyclePayload(const uint8_t *eightBytes, SpaFilterCycleSettings &out)
{
  if (eightBytes == nullptr)
  {
    return false;
  }
  out.filt1Hour = eightBytes[0];
  out.filt1Minute = eightBytes[1];
  out.filt1DurHour = eightBytes[2];
  out.filt1DurMinute = eightBytes[3];
  out.filt2Enable = (eightBytes[4] & 0x80) != 0;
  out.filt2Hour = eightBytes[4] & 0x7F;
  out.filt2Minute = eightBytes[5];
  out.filt2DurHour = eightBytes[6];
  out.filt2DurMinute = eightBytes[7];
  if (!out.filt2Enable && eightBytes[4] == 0x00)
  {
    out.filt2Hour = 0;
    out.filt2Minute = 0;
    out.filt2DurHour = 0;
    out.filt2DurMinute = 0;
  }
  const char *reason = nullptr;
  return spaValidateFilterCycleSettings(out, &reason);
}

namespace
{

bool parseBoolPayload(const String &payload, bool &out)
{
  String t = payload;
  t.trim();
  if (t.equalsIgnoreCase("true") || t.equalsIgnoreCase("on") || t == "1")
  {
    out = true;
    return true;
  }
  if (t.equalsIgnoreCase("false") || t.equalsIgnoreCase("off") || t == "0")
  {
    out = false;
    return true;
  }
  return false;
}

void overlayFilter1Json(JsonObjectConst f1, SpaFilterCycleSettings &out)
{
  if (f1.containsKey("startHour"))
  {
    out.filt1Hour = f1["startHour"] | 0;
  }
  if (f1.containsKey("startMinute"))
  {
    out.filt1Minute = f1["startMinute"] | 0;
  }
  if (f1.containsKey("durationHour"))
  {
    out.filt1DurHour = f1["durationHour"] | 0;
  }
  if (f1.containsKey("durationMinute"))
  {
    out.filt1DurMinute = f1["durationMinute"] | 0;
  }
}

void overlayFilter2Json(JsonObjectConst f2, SpaFilterCycleSettings &out)
{
  if (f2.containsKey("enabled"))
  {
    out.filt2Enable = f2["enabled"] | false;
  }
  if (f2.containsKey("startHour"))
  {
    out.filt2Hour = f2["startHour"] | 0;
  }
  if (f2.containsKey("startMinute"))
  {
    out.filt2Minute = f2["startMinute"] | 0;
  }
  if (f2.containsKey("durationHour"))
  {
    out.filt2DurHour = f2["durationHour"] | 0;
  }
  if (f2.containsKey("durationMinute"))
  {
    out.filt2DurMinute = f2["durationMinute"] | 0;
  }
}

} // namespace

bool spaFilterSettingsFromCache(SpaFilterCycleSettings &out)
{
  if (spaFilterSettingsData.lastUpdate == 0)
  {
    return false;
  }
  out.filt1Hour = spaFilterSettingsData.filt1Hour;
  out.filt1Minute = spaFilterSettingsData.filt1Minute;
  out.filt1DurHour = spaFilterSettingsData.filt1DurationHour;
  out.filt1DurMinute = spaFilterSettingsData.filt1DurationMinute;
  out.filt2Enable = spaFilterSettingsData.filt2Enable;
  out.filt2Hour = spaFilterSettingsData.filt2Hour;
  out.filt2Minute = spaFilterSettingsData.filt2Minute;
  out.filt2DurHour = spaFilterSettingsData.filt2DurationHour;
  out.filt2DurMinute = spaFilterSettingsData.filt2DurationMinute;
  return true;
}

bool spaParseFilterTimeHm(const String &payload, uint8_t &hour, uint8_t &minute)
{
  String t = payload;
  t.trim();
  if (t.length() < 4 || t.length() > 5)
  {
    return false;
  }
  const int colon = t.indexOf(':');
  if (colon <= 0 || colon >= t.length() - 2)
  {
    return false;
  }
  const String hourStr = t.substring(0, colon);
  const String minStr = t.substring(colon + 1);
  if (minStr.length() != 2)
  {
    return false;
  }
  for (unsigned int i = 0; i < hourStr.length(); ++i)
  {
    if (hourStr[i] < '0' || hourStr[i] > '9')
    {
      return false;
    }
  }
  for (unsigned int i = 0; i < minStr.length(); ++i)
  {
    if (minStr[i] < '0' || minStr[i] > '9')
    {
      return false;
    }
  }
  const int h = hourStr.toInt();
  const int m = minStr.toInt();
  if (h < 0 || h > 23 || m < 0 || m > 59)
  {
    return false;
  }
  hour = static_cast<uint8_t>(h);
  minute = static_cast<uint8_t>(m);
  return true;
}

bool spaParseFilterCycleJson(JsonObjectConst doc, SpaFilterCycleSettings &out, bool mergeFromCache, const char **errReason)
{
  if (mergeFromCache)
  {
    if (!spaFilterSettingsFromCache(out))
    {
      if (errReason)
      {
        *errReason = "filter_settings_not_ready";
      }
      return false;
    }
  }
  else
  {
    memset(&out, 0, sizeof(out));
  }

  JsonObjectConst f1;
  JsonObjectConst f2;
  if (doc.containsKey("filter"))
  {
    JsonObjectConst filter = doc["filter"];
    f1 = filter["filter1"];
    f2 = filter["filter2"];
  }
  else
  {
    f1 = doc["filter1"];
    f2 = doc["filter2"];
  }

  if (!mergeFromCache && (f1.isNull() || f2.isNull()))
  {
    if (errReason)
    {
      *errReason = "invalid_filter_payload";
    }
    return false;
  }

  if (!f1.isNull())
  {
    overlayFilter1Json(f1, out);
  }
  if (!f2.isNull())
  {
    overlayFilter2Json(f2, out);
  }

  spaNormalizeFilter2Schedule(out);
  return spaValidateFilterCycleSettings(out, errReason);
}

bool spaApplyFilterGranularMqtt(const String &subPath, const String &payload, SpaFilterCycleSettings &out, const char **errReason)
{
  if (!spaFilterSettingsFromCache(out))
  {
    if (errReason)
    {
      *errReason = "filter_settings_not_ready";
    }
    return false;
  }

  if (subPath == "filter/filter1/start")
  {
    uint8_t hour = 0;
    uint8_t minute = 0;
    if (!spaParseFilterTimeHm(payload, hour, minute))
    {
      if (errReason)
      {
        *errReason = "invalid_time_payload";
      }
      return false;
    }
    out.filt1Hour = hour;
    out.filt1Minute = minute;
  }
  else if (subPath == "filter/filter1/duration")
  {
    uint8_t hour = 0;
    uint8_t minute = 0;
    if (!spaParseFilterTimeHm(payload, hour, minute))
    {
      if (errReason)
      {
        *errReason = "invalid_time_payload";
      }
      return false;
    }
    out.filt1DurHour = hour;
    out.filt1DurMinute = minute;
  }
  else if (subPath == "filter/filter2/start")
  {
    uint8_t hour = 0;
    uint8_t minute = 0;
    if (!spaParseFilterTimeHm(payload, hour, minute))
    {
      if (errReason)
      {
        *errReason = "invalid_time_payload";
      }
      return false;
    }
    out.filt2Hour = hour;
    out.filt2Minute = minute;
    out.filt2Enable = true;
  }
  else if (subPath == "filter/filter2/duration")
  {
    uint8_t hour = 0;
    uint8_t minute = 0;
    if (!spaParseFilterTimeHm(payload, hour, minute))
    {
      if (errReason)
      {
        *errReason = "invalid_time_payload";
      }
      return false;
    }
    out.filt2DurHour = hour;
    out.filt2DurMinute = minute;
    out.filt2Enable = true;
  }
  else if (subPath == "filter/filter2/enabled")
  {
    bool enabled = false;
    if (!parseBoolPayload(payload, enabled))
    {
      if (errReason)
      {
        *errReason = "invalid_filter2_enabled_payload";
      }
      return false;
    }
    out.filt2Enable = enabled;
    if (!enabled)
    {
      spaNormalizeFilter2Schedule(out);
    }
  }
  else
  {
    if (errReason)
    {
      *errReason = "unsupported_command";
    }
    return false;
  }

  spaNormalizeFilter2Schedule(out);
  return spaValidateFilterCycleSettings(out, errReason);
}

bool spaFilterCycleSettingsEqual(const SpaFilterCycleSettings &a, const SpaFilterSettingsData &cached)
{
  if (a.filt1Hour != cached.filt1Hour || a.filt1Minute != cached.filt1Minute ||
      a.filt1DurHour != cached.filt1DurationHour || a.filt1DurMinute != cached.filt1DurationMinute)
  {
    return false;
  }
  if (a.filt2Enable != cached.filt2Enable)
  {
    return false;
  }
  if (!a.filt2Enable)
  {
    return true;
  }
  return a.filt2Hour == cached.filt2Hour && a.filt2Minute == cached.filt2Minute &&
         a.filt2DurHour == cached.filt2DurationHour && a.filt2DurMinute == cached.filt2DurationMinute;
}

bool spaFilter1Running()
{
  return spaStatusData.filterMode == 1 || spaStatusData.filterMode == 3;
}

bool spaFilter2Running()
{
  return spaStatusData.filterMode == 2 || spaStatusData.filterMode == 3;
}

SpaCommandResult spaSetFilterCycles(const SpaFilterCycleSettings &settings, SpaCommandSource source)
{
  if (!spaHasFreshStatus())
  {
    return {false, SPA_COMMAND_NOT_READY, "spa status not ready"};
  }
  if (spaFilterSettingsData.lastUpdate == 0)
  {
    return {false, SPA_COMMAND_NOT_READY, "filter_settings_not_ready"};
  }

  SpaFilterCycleSettings normalized = settings;
  spaNormalizeFilter2Schedule(normalized);

  const char *reason = nullptr;
  if (!spaValidateFilterCycleSettings(normalized, &reason))
  {
    return {false, SPA_COMMAND_INVALID_ARGUMENT, reason ? reason : "invalid_filter_payload"};
  }

  if (spaFilterCycleSettingsEqual(normalized, spaFilterSettingsData))
  {
    return {true, SPA_COMMAND_ACCEPTED, "accepted"};
  }

  uint8_t filt2HourByte = 0x00;
  if (normalized.filt2Enable)
  {
    filt2HourByte = (uint8_t)((normalized.filt2Hour & 0x7F) | 0x80);
  }

  CircularBuffer<uint8_t, BALBOA_MESSAGE_SIZE> frame;
  frame.push(destinationId());
  frame.push(kBroadcastChannel);
  frame.push(Filter_Cycles_Type);
  frame.push(normalized.filt1Hour);
  frame.push(normalized.filt1Minute);
  frame.push(normalized.filt1DurHour);
  frame.push(normalized.filt1DurMinute);
  frame.push(filt2HourByte);
  frame.push(normalized.filt2Enable ? normalized.filt2Minute : 0);
  frame.push(normalized.filt2Enable ? normalized.filt2DurHour : 0);
  frame.push(normalized.filt2Enable ? normalized.filt2DurMinute : 0);

  SpaCommandResult result = queueFrame(frame, "set_filter_cycles", source);
  if (result.accepted)
  {
    spaRequestFilterSettings();
    spaScheduleFilterSettingsReadbackFollowup(2);
  }
  return result;
}

SpaCommandResult spaSetSpaPanelClockTime(uint8_t hour24, uint8_t minute, SpaCommandSource source)
{
  if (!spaHasFreshStatus())
  {
    return {false, SPA_COMMAND_NOT_READY, "spa status not ready"};
  }
  if (minute > 59)
  {
    return {false, SPA_COMMAND_INVALID_ARGUMENT, "minute out of range"};
  }
  if (hour24 > 23)
  {
    return {false, SPA_COMMAND_INVALID_ARGUMENT, "hour out of range"};
  }

  // protocol.md: Set Time 0x21 payload HH MM; bit 7 of HH selects 24-hour panel format.
  uint8_t hourByte = hour24 & 0x7F;
  if ((spaStatusData.clockMode & 0x02) != 0)
  {
    hourByte |= 0x80;
  }

  CircularBuffer<uint8_t, BALBOA_MESSAGE_SIZE> frame;
  frame.push(destinationId());
  frame.push(kBroadcastChannel);
  frame.push(Set_Time_Type);
  frame.push(hourByte);
  frame.push(minute);
  return queueFrame(frame, "set_time", source);
}

SpaCommandResult spaSetSpaPanelClockTimeEx(uint8_t hour24, uint8_t minute, bool use24Hour, SpaCommandSource source)
{
  if (!spaHasFreshStatus())
  {
    return {false, SPA_COMMAND_NOT_READY, "spa status not ready"};
  }
  if (minute > 59)
  {
    return {false, SPA_COMMAND_INVALID_ARGUMENT, "minute out of range"};
  }
  if (hour24 > 23)
  {
    return {false, SPA_COMMAND_INVALID_ARGUMENT, "hour out of range"};
  }

  uint8_t hourByte = hour24 & 0x7F;
  if (use24Hour)
  {
    hourByte |= 0x80;
  }

  CircularBuffer<uint8_t, BALBOA_MESSAGE_SIZE> frame;
  frame.push(destinationId());
  frame.push(kBroadcastChannel);
  frame.push(Set_Time_Type);
  frame.push(hourByte);
  frame.push(minute);
  return queueFrame(frame, "set_time_ex", source);
}

SpaCommandResult spaSendToggleDiagnostic(
    uint8_t itemCode,
    bool useWifiDestination,
    bool includeZeroPad,
    SpaCommandSource source,
    String *outFrameHex)
{
  if (!spaHasFreshStatus())
  {
    return {false, SPA_COMMAND_NOT_READY, "spa status not ready"};
  }

  uint8_t dest = destinationIdForMode(useWifiDestination);
  if (dest == 0)
  {
    return {false, SPA_COMMAND_INVALID_ARGUMENT, "invalid destination id"};
  }

  CircularBuffer<uint8_t, BALBOA_MESSAGE_SIZE> frame;
  frame.push(dest);
  frame.push(kBroadcastChannel);
  frame.push(Toggle_Item_Request_Type);
  frame.push(itemCode);
  if (includeZeroPad)
  {
    frame.push(0x00);
  }

  return queueFrame(frame, includeZeroPad ? "toggle_diag_pad00" : "toggle_diag_nopad", source, outFrameHex);
}

SpaCommandResult spaSendToggleOnNextCtsDiagnostic(
    uint8_t itemCode,
    bool useWifiDestination,
    bool includeZeroPad,
    SpaCommandSource source,
    String *outFrameHex,
    uint32_t *outArmCount)
{
  if (!spaHasFreshStatus())
  {
    return {false, SPA_COMMAND_NOT_READY, "spa status not ready"};
  }

  uint8_t dest = destinationIdForMode(useWifiDestination);
  if (dest == 0)
  {
    return {false, SPA_COMMAND_INVALID_ARGUMENT, "invalid destination id"};
  }

  CircularBuffer<uint8_t, BALBOA_MESSAGE_SIZE> frame;
  frame.push(dest);
  frame.push(kBroadcastChannel);
  frame.push(Toggle_Item_Request_Type);
  frame.push(itemCode);
  if (includeZeroPad)
  {
    frame.push(0x00);
  }
  addCRC(frame);
  if (outFrameHex != nullptr)
  {
    *outFrameHex = msgToString(frame);
  }

  uint8_t raw[BALBOA_MESSAGE_SIZE];
  int len = frame.size();
  for (int i = 0; i < len; i++)
  {
    raw[i] = frame[i];
  }
  if (!rs485ArmFrameOnNextCts(raw, len, outArmCount))
  {
#ifdef LOCAL_CLIENT
    if (!rs485UartBegun() || rs485SafeModeActive())
    {
      return {false, SPA_COMMAND_NOT_READY, "rs485_uart_not_ready"};
    }
#endif
    return {false, SPA_COMMAND_INVALID_ARGUMENT, "failed to arm next_cts frame"};
  }
  Log.verbose(F("[Cmd ]: armed next_cts toggle from %s: %s" CR), sourceLabel(source), msgToString(frame).c_str());
  return {true, SPA_COMMAND_ACCEPTED, "armed_next_cts"};
}

namespace
{
bool parsePanelClockHHMM(const char *t, uint8_t &hour24, uint8_t &minute)
{
  if (t == nullptr || strlen(t) < 5 || t[2] != ':')
  {
    return false;
  }
  if (t[0] < '0' || t[0] > '9' || t[1] < '0' || t[1] > '9' ||
      t[3] < '0' || t[3] > '9' || t[4] < '0' || t[4] > '9')
  {
    return false;
  }
  hour24 = (uint8_t)((t[0] - '0') * 10 + (t[1] - '0'));
  minute = (uint8_t)((t[3] - '0') * 10 + (t[4] - '0'));
  return hour24 <= 23 && minute <= 59;
}

int circularMinuteDrift(uint8_t h1, uint8_t m1, uint8_t h2, uint8_t m2)
{
  const int t1 = (int)h1 * 60 + (int)m1;
  const int t2 = (int)h2 * 60 + (int)m2;
  int diff = abs(t1 - t2);
  if (diff > 720)
  {
    diff = 1440 - diff;
  }
  return diff;
}

bool gatewayLocalHHMM(uint8_t &hour24, uint8_t &minute)
{
  time_t t = getTime();
  if (t <= 0)
  {
    return false;
  }
  struct tm tmStore;
  struct tm *p = localtime_r(&t, &tmStore);
  if (p == nullptr)
  {
    return false;
  }
  hour24 = (uint8_t)p->tm_hour;
  minute = (uint8_t)p->tm_min;
  return true;
}

constexpr unsigned long kPanelClockAutoSyncBootDelayMs = 45000UL;

enum PanelClockAutoSyncPhase
{
  kAutoSyncPending = 0,
  kAutoSyncWaitingDelay = 1,
  kAutoSyncDone = 2,
};

struct PanelClockAutoSyncBootRecord
{
  bool complete = false;
  const char *action = nullptr;
  int driftMin = -1;
  const char *reason = nullptr;
  bool commandAccepted = false;
};

PanelClockAutoSyncPhase autoSyncPhase = kAutoSyncPending;
unsigned long autoSyncDelayStartMs = 0;
PanelClockAutoSyncBootRecord autoSyncBootRecord;

void autoSyncRecord(const char *action, int driftMin, const char *reason, bool commandAccepted)
{
  autoSyncBootRecord.complete = true;
  autoSyncBootRecord.action = action;
  autoSyncBootRecord.driftMin = driftMin;
  autoSyncBootRecord.reason = reason;
  autoSyncBootRecord.commandAccepted = commandAccepted;
}

void autoSyncAttempt()
{
  uint8_t panelHour = 0;
  uint8_t panelMinute = 0;
  if (!parsePanelClockHHMM(spaStatusData.time, panelHour, panelMinute))
  {
    autoSyncRecord("failed", -1, "panel_time_unavailable", false);
    Log.notice(F("[Cmd ]: panel clock auto-sync skipped: panel time unavailable" CR));
    return;
  }

  uint8_t gatewayHour = 0;
  uint8_t gatewayMinute = 0;
  if (!gatewayLocalHHMM(gatewayHour, gatewayMinute))
  {
    autoSyncRecord("failed", -1, "gateway_clock_unavailable", false);
    Log.notice(F("[Cmd ]: panel clock auto-sync skipped: gateway clock unavailable" CR));
    return;
  }

  const int driftMin = circularMinuteDrift(panelHour, panelMinute, gatewayHour, gatewayMinute);
  if (driftMin <= AUTO_SYNC_PANEL_CLOCK_THRESHOLD_MIN)
  {
    autoSyncRecord("skipped", driftMin, "within_threshold", false);
    Log.notice(F("[Cmd ]: panel clock auto-sync skipped: drift %d min (threshold %d); panel %02d:%02d gateway %02d:%02d" CR),
               driftMin, AUTO_SYNC_PANEL_CLOCK_THRESHOLD_MIN, panelHour, panelMinute, gatewayHour, gatewayMinute);
    return;
  }

  SpaCommandResult result = spaSetSpaPanelClockTime(gatewayHour, gatewayMinute, SPA_COMMAND_SOURCE_AUTO);
  if (result.accepted)
  {
    autoSyncRecord("synced", driftMin, result.reason, true);
    Log.notice(F("[Cmd ]: panel clock auto-sync sent: drift %d min; panel %02d:%02d -> gateway %02d:%02d" CR),
               driftMin, panelHour, panelMinute, gatewayHour, gatewayMinute);
  }
  else
  {
    autoSyncRecord("failed", driftMin, result.reason, false);
    Log.notice(F("[Cmd ]: panel clock auto-sync failed: drift %d min; %s" CR), driftMin, result.reason);
  }
}
} // namespace

void spaPanelClockAutoSyncTick()
{
#if defined(LOCAL_CLIENT) && AUTO_SYNC_PANEL_CLOCK
  if (autoSyncPhase == kAutoSyncDone)
  {
    return;
  }

  if (WiFi.status() != WL_CONNECTED || getTime() <= 0 || !spaHasFreshStatus())
  {
    return;
  }

  if (autoSyncPhase == kAutoSyncPending)
  {
    autoSyncDelayStartMs = millis();
    autoSyncPhase = kAutoSyncWaitingDelay;
    return;
  }

  if (autoSyncPhase == kAutoSyncWaitingDelay)
  {
    if (millis() - autoSyncDelayStartMs < kPanelClockAutoSyncBootDelayMs)
    {
      return;
    }
    autoSyncPhase = kAutoSyncDone;
    autoSyncAttempt();
  }
#endif
}

void spaPanelClockAutoSyncAppendToJson(JsonObject root)
{
#if defined(LOCAL_CLIENT)
  JsonObject sync = root.createNestedObject("panelClockAutoSync");
  sync["enabled"] = (AUTO_SYNC_PANEL_CLOCK != 0);
  sync["thresholdMin"] = AUTO_SYNC_PANEL_CLOCK_THRESHOLD_MIN;
  if (autoSyncBootRecord.complete)
  {
    JsonObject lastBoot = sync.createNestedObject("lastBoot");
    lastBoot["action"] = autoSyncBootRecord.action;
    if (autoSyncBootRecord.driftMin >= 0)
    {
      lastBoot["driftMin"] = autoSyncBootRecord.driftMin;
    }
    if (autoSyncBootRecord.reason != nullptr)
    {
      lastBoot["reason"] = autoSyncBootRecord.reason;
    }
    lastBoot["commandAccepted"] = autoSyncBootRecord.commandAccepted;
  }
#endif
}
