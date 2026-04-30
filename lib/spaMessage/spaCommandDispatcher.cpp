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
