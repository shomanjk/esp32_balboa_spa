#include "cacheRead.h"
#include <Arduino.h>
#include <CircularBuffer.hpp>
#include <ArduinoLog.h>

#include  <spaUtilities.h>
#include <bridge.h>
#include <diagBridgeLog.h>

#include "balboa.h"
#include "spaMessage.h"

// Local Functions

void processFragment(uint8_t *data, size_t length);

/** Indices `data[4]` / `data[5]` require this minimum; max must fit `SpaWriteQueueMessage.message`. */
static constexpr size_t kBridgeFrameMin = 6u;

// 7e 08 0a bf 22 00 00 01 58 7e 7e 08 0a bf 22 04 00 00 f4 7e 7e 08 0a bf 22 01 00 00 34 7e 7e 08 0a bf 22 02 00 00 89 7e
// config request (2e), followed by 04 (25), Filter Cycles Message (23), and Information Response ( 24 )

void cacheRead(uint8_t *data, size_t length)
{
  BRIDGE_LOG_NOISY(F("[BridgeDiag]: cacheRead len=%u" CR), static_cast<unsigned int>(length));
  size_t pos = 0;
  while (pos < length)
  {
    if (data[pos] == 0x7E)
    {
      if (pos + 1 >= length)
      {
        break;
      }
      const uint8_t fragmentLength = data[pos + 1];
      const size_t frameLen = static_cast<size_t>(fragmentLength) + 2u;
      if (frameLen < kBridgeFrameMin || frameLen > static_cast<size_t>(BALBOA_MESSAGE_SIZE))
      {
        Log.verbose(F("[Bridge]: cacheRead resync at %u (len byte=%u → frameLen=%u)" CR),
                    static_cast<unsigned int>(pos),
                    static_cast<unsigned int>(fragmentLength),
                    static_cast<unsigned int>(frameLen));
        pos++;
        continue;
      }
      if (pos + frameLen > length)
      {
        break;
      }
      processFragment(&data[pos], frameLen);
      pos += frameLen;
    }
    else
    {
      pos++;
    }
  }
}

#define cacheRecent(structure)                                                                                                                               \
  {                                                                                                                                                          \
    unsigned long currentTime = getTime();                                                                                                                   \
    ((currentTime - (structure).lastUpdate < STALE_TIME * 2) ? bridgeSend((structure).rawData, (structure).rawDataLength) : sendMessageToSpa(data, length)); \
  }

void processFragment(uint8_t *data, size_t length)
{
  if (length < kBridgeFrameMin || length > static_cast<size_t>(BALBOA_MESSAGE_SIZE))
  {
    BRIDGE_LOG_NOISY(F("[BridgeDiag]: drop fragment len=%u" CR), static_cast<unsigned int>(length));
    return;
  }
  const String frameHex = msgToString(data, length);
  BRIDGE_LOG_NOISY(F("[BridgeDiag]: fragment type=0x%x len=%u frame=%s" CR),
                   data[4],
                   static_cast<unsigned int>(length),
                   frameHex.c_str());
  // Check if the fragment is a valid message
  if (data[4] == Settings_Request_Type)
  {
    // Check the message type
    switch (data[5])
    {
    case Configuration_Req_Type:
      Log.verbose(F("[Cache]: Configuration Message" CR));
      cacheRecent(spaConfigurationData);
      break;
    case Settings_0x04_Req_Type:
      Log.verbose(F("[Cache]: Settings 0x04 Message" CR));
      cacheRecent(spaSettings0x04Data);
      break;
    case Filter_Cycles_Req_Type:
      Log.verbose(F("[Cache]: Filter Cycles Message" CR));
      cacheRecent(spaFilterSettingsData);
      break;
    case Information_Req_Type:
      Log.verbose(F("[Cache]: Information Request" CR));
      cacheRecent(spaInformationData);
      break;
    default:
      Log.verbose(F("[Cache]: Unknown Request %x" CR), data[5]);
      BRIDGE_LOG_NOISY(F("[BridgeDiag]: action=sendMessageToSpa reason=unknown_settings_request code=0x%x" CR), data[5]);
      sendMessageToSpa(data, length);
      break;
    }
  }
  else
  {
    BRIDGE_LOG_NOISY(F("[BridgeDiag]: action=sendMessageToSpa reason=pass_through type=0x%x" CR), data[4]);
    sendMessageToSpa(data, length);
  }
}