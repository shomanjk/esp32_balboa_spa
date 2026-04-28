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
const uint8_t kSof = 0x7E;
const uint8_t kEof = 0x7E;
const uint8_t kCrcPolynomial = 0x07;

const float kFallbackMinTempF = 50.0f;
const float kFallbackMaxTempF = 110.0f;
const float kFallbackMinTempC = 10.0f;
const float kFallbackMaxTempC = 43.0f;

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

uint8_t crc8(CircularBuffer<uint8_t, BALBOA_MESSAGE_SIZE> &data)
{
  uint8_t crc = 0xB5;

  for (unsigned int cur = 0; cur < data.size(); cur++)
  {
    for (unsigned int i = 0x80; i != 0; i /= 2)
    {
      bool bit = crc & 0x80;
      if (data[cur] & i)
      {
        bit = !bit;
      }
      crc <<= 1;
      if (bit)
      {
        crc ^= kCrcPolynomial;
      }
    }
    crc &= 0xFF;
  }
  return crc ^ 0x02;
}

void addFramingAndCrc(CircularBuffer<uint8_t, BALBOA_MESSAGE_SIZE> &data)
{
  data.unshift(data.size() + 2);
  data.push(crc8(data));
  data.unshift(kSof);
  data.push(kEof);
}

SpaCommandResult queueFrame(CircularBuffer<uint8_t, BALBOA_MESSAGE_SIZE> &frame, const char *logAction, SpaCommandSource source, String *outFrameHex = nullptr)
{
  addFramingAndCrc(frame);
  if (outFrameHex != nullptr)
  {
    *outFrameHex = msgToString(frame);
  }
  sendMessageToSpa(frame);
  Log.verbose(F("[Cmd ]: accepted %s from %s: %s" CR), logAction, sourceLabel(source), msgToString(frame).c_str());
  return {true, SPA_COMMAND_ACCEPTED, "accepted"};
}

bool hasValidTempBounds(float &minTemp, float &maxTemp)
{
  minTemp = (spaStatusData.tempScale ? kFallbackMinTempC : kFallbackMinTempF);
  maxTemp = (spaStatusData.tempScale ? kFallbackMaxTempC : kFallbackMaxTempF);

  if (spaStatusData.lowSetTemp > 0 && spaStatusData.highSetTemp > 0 && spaStatusData.highSetTemp >= spaStatusData.lowSetTemp)
  {
    minTemp = spaStatusData.lowSetTemp;
    maxTemp = spaStatusData.highSetTemp;
    return true;
  }
  return false;
}
} // namespace

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

  float minTemp = 0;
  float maxTemp = 0;
  hasValidTempBounds(minTemp, maxTemp);
  if (targetTemperature < minTemp || targetTemperature > maxTemp)
  {
    return {false, SPA_COMMAND_INVALID_ARGUMENT, "target out of range"};
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
