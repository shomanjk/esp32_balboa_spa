#include "spaCommandDispatcher.h"

#include <ArduinoLog.h>
#include <CircularBuffer.hpp>
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
  sendMessageToSpa(frame);
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

bool spaCanAcceptCommands()
{
  return !staleData(spaStatusData) && !staleData(spaConfigurationData);
}

SpaCommandResult spaSendToggleCommand(uint8_t itemCode, SpaCommandSource source)
{
  if (!spaCanAcceptCommands())
  {
    return {false, SPA_COMMAND_NOT_READY, "spa status/config not ready"};
  }

  CircularBuffer<uint8_t, BALBOA_MESSAGE_SIZE> frame;
  frame.push(destinationId());
  frame.push(kBroadcastChannel);
  frame.push(Toggle_Item_Request_Type);
  frame.push(itemCode);
  frame.push(0x00);
  return queueFrame(frame, "toggle", source);
}

SpaCommandResult spaSetTargetTemperature(float targetTemperature, SpaCommandSource source)
{
  if (!spaCanAcceptCommands())
  {
    return {false, SPA_COMMAND_NOT_READY, "spa status/config not ready"};
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

SpaCommandResult spaSetSpaPanelClockTime(uint8_t hour24, uint8_t minute, SpaCommandSource source)
{
  if (!spaCanAcceptCommands())
  {
    return {false, SPA_COMMAND_NOT_READY, "spa status/config not ready"};
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

SpaCommandResult spaSendToggleDiagnostic(
    uint8_t itemCode,
    bool useWifiDestination,
    bool includeZeroPad,
    SpaCommandSource source,
    String *outFrameHex)
{
  if (!spaCanAcceptCommands())
  {
    return {false, SPA_COMMAND_NOT_READY, "spa status/config not ready"};
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
  if (!spaCanAcceptCommands())
  {
    return {false, SPA_COMMAND_NOT_READY, "spa status/config not ready"};
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
    return {false, SPA_COMMAND_INVALID_ARGUMENT, "failed to arm next_cts frame"};
  }
  Log.verbose(F("[Cmd ]: armed next_cts toggle from %s: %s" CR), sourceLabel(source), msgToString(frame).c_str());
  return {true, SPA_COMMAND_ACCEPTED, "armed_next_cts"};
}
