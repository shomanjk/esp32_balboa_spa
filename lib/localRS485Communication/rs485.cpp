#include <Arduino.h>
#include <CircularBuffer.hpp>
#include <ArduinoLog.h>
#include <esp_task_wdt.h>
#include <cstring>
#if defined(ARDUINO_ARCH_ESP32)
#include "driver/uart.h"
#endif

#include  <spaUtilities.h>
#include <spaMessage.h>

#include "rs485.h"
#include "../../src/config.h"
#include "../../src/main.h"
#include "../../src/rs485_led_hooks.h"
#include <diagBridgeLog.h>

// QueueHandle_t rs485WriteQueue;

// Local functions

void rs485Write(CircularBuffer<uint8_t, BALBOA_MESSAGE_SIZE> &data);
bool isMessageValid(CircularBuffer<uint8_t, BALBOA_MESSAGE_SIZE> &data);
void sendExistingClientResponse(uint8_t id);
void applyRs485Polarity(bool inverted);
const char *rs485ModeName(bool inverted);
void rs485DrainUart();
void rs485ProcessByte(uint8_t x, uint8_t uartAvailable);
void rs485RecordRawByte(uint8_t value, uint8_t uartAvailable);
// bool hasDayChanged();

RTC_NOINIT_ATTR Rs485Stats rs485Stats;

CircularBuffer<uint8_t, BALBOA_MESSAGE_SIZE> spaMessage;
uint8_t id = 0x00; // spa id
bool rs485PolarityInverted = false;
bool rs485PolarityLocked = false;
uint8_t rs485PolarityDetectPhase = 0; // 0: try normal, 1: try inverted, 2: detection complete
uint32_t rs485PolarityDetectWindowStartMs = 0;
uint32_t rs485ValidFramesSinceBoot = 0;
uint32_t rs485LastSampleMs = 0;
Rs485Snapshot rs485History[RS485_HISTORY_SIZE];
uint16_t rs485HistoryHead = 0;
uint16_t rs485HistoryCount = 0;
Rs485RawByte rs485RawCapture[RS485_RAW_CAPTURE_SIZE];
uint16_t rs485RawCaptureHead = 0;
uint16_t rs485RawCaptureCount = 0;
uint32_t rs485LastRawCaptureMs = 0;
uint32_t rs485LastCtsAtMs = 0;
uint32_t rs485CtsSeenCount = 0;
uint32_t rs485NextCtsArmSeenCount = 0;
uint32_t rs485NextCtsFireSeenCount = 0;
bool rs485NextCtsArmed = false;
uint8_t rs485NextCtsFrame[BALBOA_MESSAGE_SIZE];
int rs485NextCtsFrameLength = 0;

#ifndef TX485_Tx
#define TX485_Tx 17
#endif

#ifndef TX485_Rx
#define TX485_Rx 16
#endif

#ifndef RS485_SERIAL_PORT
#define RS485_SERIAL_PORT Serial2
#endif
#ifndef RS485_UART_NUM
#define RS485_UART_NUM UART_NUM_2
#endif

#define RS_485_MAGIC_NUMBER 0x21345679
#define RS485_POLARITY_DETECT_WINDOW_MS 15000
#define RS485_BAUD_RATE 115200

time_t lastCheckedTime;

void rs485Setup()
{
  if (!AUTO_TX)
  {
    pinMode(TX485_Tx, OUTPUT);
    digitalWrite(TX485_Tx, LOW);
  }
  // Spa communication, 115.200 baud 8N1
  applyRs485Polarity(false);
  Log.verbose(F("[rs485]: RS485 setup, RX GPIO %d, TX GPIO %d, auto polarity detect %s" CR), TX485_Rx, TX485_Tx, "enabled");

  lastCheckedTime = getTime();
  if (rs485Stats.magicNumber != RS_485_MAGIC_NUMBER)
  {
    Log.verbose(F("[Mess]: rs485Stats.magicNumber: %x" CR), rs485Stats.magicNumber);
    rs485Stats = {};
    rs485Stats.magicNumber = RS_485_MAGIC_NUMBER;
  }
  rs485Stats.polarityInverted = rs485PolarityInverted ? 1 : 0;
  rs485Stats.polarityLocked = rs485PolarityLocked ? 1 : 0;
};

/*

*/

void rs485Loop()
{
  if (!rs485PolarityLocked && millis() - rs485PolarityDetectWindowStartMs >= RS485_POLARITY_DETECT_WINDOW_MS)
  {
    if (rs485ValidFramesSinceBoot == 0)
    {
      rs485PolarityDetectPhase = (rs485PolarityDetectPhase == 0 ? 1 : 0);
      rs485Stats.polaritySwitchesToday++;
      applyRs485Polarity(rs485PolarityDetectPhase == 1);
      spaMessage.clear();
      Log.warning(F("[rs485]: No valid frames yet, retrying with %s mode (RX/TX inversion toggled)" CR),
                  rs485ModeName(rs485PolarityInverted));
    }
  }

  rs485DrainUart();

  if (hasDayChanged(lastCheckedTime))
  {
    rs485Stats.rawBytesYesterday = rs485Stats.rawBytesToday;
    rs485Stats.rawBytesToday = 0;
    rs485Stats.rawBytesNormalYesterday = rs485Stats.rawBytesNormalToday;
    rs485Stats.rawBytesNormalToday = 0;
    rs485Stats.rawBytesInvertedYesterday = rs485Stats.rawBytesInvertedToday;
    rs485Stats.rawBytesInvertedToday = 0;
    rs485Stats.framesYesterday = rs485Stats.framesToday;
    rs485Stats.framesToday = 0;
    rs485Stats.messagesYesterday = rs485Stats.messagesToday;
    rs485Stats.crcYesterday = rs485Stats.crcToday;
    rs485Stats.messagesToday = 0;
    rs485Stats.crcToday = 0;
    rs485Stats.badFormatYesterday = rs485Stats.badFormatToday;
    rs485Stats.badFormatToday = 0;
    rs485Stats.polaritySwitchesYesterday = rs485Stats.polaritySwitchesToday;
    rs485Stats.polaritySwitchesToday = 0;
    rs485Stats.frameMarkersYesterday = rs485Stats.frameMarkersToday;
    rs485Stats.frameMarkersToday = 0;
    rs485Stats.maxUartAvailableYesterday = rs485Stats.maxUartAvailableToday;
    rs485Stats.maxUartAvailableToday = 0;
    rs485Stats.rawCaptureOverflowsYesterday = rs485Stats.rawCaptureOverflowsToday;
    rs485Stats.rawCaptureOverflowsToday = 0;
  }

  rs485SampleHistory();
};

void rs485DrainUart()
{
  const uint16_t maxBytesPerLoop = 512;
  uint16_t processed = 0;
  while (RS485_SERIAL_PORT.available() && processed < maxBytesPerLoop)
  {
    int uartAvailable = RS485_SERIAL_PORT.available();
    if (uartAvailable > static_cast<int>(rs485Stats.maxUartAvailableToday))
    {
      rs485Stats.maxUartAvailableToday = uartAvailable;
    }
    uint8_t x = RS485_SERIAL_PORT.read();
    if (uartAvailable > 255)
    {
      uartAvailable = 255;
    }
    rs485ProcessByte(x, static_cast<uint8_t>(uartAvailable));
    processed++;
  }
}

void rs485ProcessByte(uint8_t x, uint8_t uartAvailable)
{
  rs485RecordRawByte(x, uartAvailable);
  rs485Stats.rawBytesToday++;
  if (rs485PolarityInverted)
  {
    rs485Stats.rawBytesInvertedToday++;
  }
  else
  {
    rs485Stats.rawBytesNormalToday++;
  }
  rs485Stats.lastByteMs = millis();
  if (x == 0x7E)
  {
    rs485Stats.frameMarkersToday++;
  }
  rs485LedNotifyRx();
  spaMessage.push(x);

  // Drop until SOF is seen
  if (spaMessage.first() != 0x7E)
    spaMessage.clear();
  if (spaMessage.size() > BALBOA_MESSAGE_SIZE - 1)
  {
    rs485Stats.badFormatToday++;
    Log.warning(F("[rs485]: Invalid message, too long: %s" CR), msgToString(spaMessage).c_str());
    spaMessage.clear();
  }

  // Double SOF-marker, drop last one
  if (spaMessage.size() > 1 && spaMessage[1] == 0x7E)
    spaMessage.pop();

  if (x == 0x7E && spaMessage.size() > 2)
  {
    // Log.verbose(F("[rs485]: spaMessage %s, size %d, supplied size %d, %d" CR), msgToString(spaMessage).c_str(), spaMessage.size(), spaMessage[1] + 2, isMessageValid(spaMessage));
  }

  if (spaMessage.size() == 4 && (spaMessage[1] > BALBOA_MESSAGE_SIZE || !(spaMessage[3] == 0xBF || spaMessage[3] == 0xAF)))
  {
    rs485Stats.badFormatToday++;
    Log.warning(F("[rs485]: Invalid message, corrupted length/broadcast flag: %s" CR), msgToString(spaMessage).c_str());
    spaMessage.clear();
  }

  if (spaMessage.size() > 1 && spaMessage.size() - 2 > spaMessage[1])
  {
    rs485Stats.badFormatToday++;
    Log.warning(F("[rs485]: Invalid message, corrupted length: %s" CR), msgToString(spaMessage).c_str());
    spaMessage.clear();
  }

  if (x == 0x7E && spaMessage.size() > 4 && spaMessage.size() == spaMessage[1] + 2)
  {
    rs485Stats.framesToday++;

    if (isMessageValid(spaMessage))
    {
      // Log.verbose(F("[rs485]: Received: %d - %s" CR), id, msgToString(spaMessage).c_str());
      rs485Stats.messagesToday++;
      rs485Stats.lastValidFrameMs = millis();
      rs485ValidFramesSinceBoot++;
      if (!rs485PolarityLocked)
      {
        rs485PolarityLocked = true;
        rs485PolarityDetectPhase = 2;
        rs485Stats.polarityLocked = 1;
        Log.notice(F("[rs485]: Polarity auto-detect locked on %s mode after first valid frame" CR),
                   rs485ModeName(rs485PolarityInverted));
      }
      if (id == 0)
      {
        if (Status_Update(spaMessage)) // This is hacky, but it appears to work
        {
          id = WIFI_MODULE_ID;
          Log.verbose(F("[rs485]: Set SPA id 0x0A" CR));
          sendExistingClientResponse(id);
          esp_task_wdt_init(RUNNING_WDT_TIMEOUT, true); // enable panic so ESP32 restarts
          spaMessage.clear();
        }

        // This method is used to assign a unique ID to the spa
        /*
            if (Channel_Assignment_Response(spaMessage))
            {
              id = spaMessage[5];
              if (id > 0x2F)
                id = 0x2F;

              ID_ack();
              mqtt.publish((mqttTopic + "node/id").c_str(), String(id, 16).c_str());
              publishDebug("Received SPA id");
              esp_task_wdt_init(RUNNING_WDT_TIMEOUT, true); // enable panic so ESP32 restarts
            }

            // FE BF 00:Any new clients?
            if (New_Client_Clear_to_Send(spaMessage))
            {
              ID_request();
            }
            */
      }
      else if (Clear_to_Send(spaMessage))
      {
        rs485ClearToSend();
      }
      else if (For_Us_Message(spaMessage))
      {
        SpaReadQueueMessage *messageToSend = new SpaReadQueueMessage;
        messageToSend->length = (spaMessage.size() < BALBOA_MESSAGE_SIZE ? spaMessage.size() : BALBOA_MESSAGE_SIZE);
        for (int i = 0; i < messageToSend->length; i++)
        {
          messageToSend->message[i] = spaMessage[i];
        }

        if (xQueueSend(spaReadQueue, &messageToSend, 0) != pdTRUE)
        {
          Log.error(F("[rs485]: SPA Read Queue full, dropped %s" CR), msgToString(messageToSend->message, messageToSend->length).c_str());
        }
        else
        {
          // Log.verbose(F("[rs485]: Data added to Read Queue [%d]%s" CR), messageToSend->length, msgToString(messageToSend->message, messageToSend->length).c_str());
        }
      }
    }
    else
    {
      Log.warning(F("[rs485]: Invalid message, crc failed: %s" CR), msgToString(spaMessage).c_str());
    }
    spaMessage.clear();
  }
}

void rs485ClearToSend()
{
  //  mqtt.publish((mqttTopic + "node/rs485Queue").c_str(), "rs485ClearToSend");
  rs485WriteQueueMessage *message;
  CircularBuffer<uint8_t, BALBOA_MESSAGE_SIZE> dataBuffer;
  BRIDGE_LOG_NOISY(F("[BridgeDiag]: cts ms=%lu depth_before=%u" CR),
             millis(),
             static_cast<unsigned int>(uxQueueMessagesWaiting(spaWriteQueue)));
  rs485LastCtsAtMs = millis();
  rs485CtsSeenCount++;
  if (rs485NextCtsArmed && rs485NextCtsFrameLength > 0)
  {
    for (int i = 0; i < rs485NextCtsFrameLength; i++)
    {
      dataBuffer.push(rs485NextCtsFrame[i]);
    }
    rs485NextCtsArmed = false;
    rs485NextCtsFireSeenCount++;
    BRIDGE_LOG_NOISY(F("[BridgeDiag]: cts_send next_cts_frame=%s cts_count=%lu" CR),
               msgToString(dataBuffer).c_str(),
               rs485CtsSeenCount);
    rs485Write(dataBuffer);
    return;
  }
  if (xQueueReceive(spaWriteQueue, &message, 0) == pdTRUE)
  {
    for (int i = 0; i < message->length; i++)
    {
      dataBuffer.push(message->message[i]);
    }
    //   mqtt.publish((mqttTopic + "node/rs485Queue").c_str(), "Queue Receive");
    BRIDGE_LOG_NOISY(F("[BridgeDiag]: cts_send queued_frame=%s depth_after_pop=%u" CR),
               msgToString(dataBuffer).c_str(),
               static_cast<unsigned int>(uxQueueMessagesWaiting(spaWriteQueue)));
    rs485Write(dataBuffer);
    delete message;
  }
  else
  {
    // A Nothing to Send message is sent by a client immediately after a
    // Clear to Send message if the client has no messages to send.
    dataBuffer.push(id);
    dataBuffer.push(0xBF);
    dataBuffer.push(0x07);
    addCRC(dataBuffer);
    //    mqtt.publish((mqttTopic + "node/rs485Queue").c_str(), "Clear to Send");
    rs485Write(dataBuffer);
  }
}

uint32_t rs485LastCtsMs()
{
  return rs485LastCtsAtMs;
}

uint32_t rs485CtsCount()
{
  return rs485CtsSeenCount;
}

uint32_t rs485NextCtsArmCount()
{
  return rs485NextCtsArmSeenCount;
}

uint32_t rs485NextCtsFireCount()
{
  return rs485NextCtsFireSeenCount;
}

bool rs485ArmFrameOnNextCts(const uint8_t *frame, int length, uint32_t *outArmCount)
{
  if (frame == nullptr || length <= 0 || length > BALBOA_MESSAGE_SIZE)
  {
    return false;
  }
  for (int i = 0; i < length; i++)
  {
    rs485NextCtsFrame[i] = frame[i];
  }
  rs485NextCtsFrameLength = length;
  rs485NextCtsArmed = true;
  rs485NextCtsArmSeenCount++;
  if (outArmCount != nullptr)
  {
    *outArmCount = rs485NextCtsArmSeenCount;
  }
  BRIDGE_LOG_NOISY(F("[BridgeDiag]: next_cts armed frame=%s arm_count=%lu" CR),
             msgToString(const_cast<uint8_t *>(frame), length).c_str(),
             rs485NextCtsArmSeenCount);
  return true;
}

inline uint8_t crc8(CircularBuffer<uint8_t, BALBOA_MESSAGE_SIZE> &data)
{
  unsigned long crc;
  int i, bit;
  uint8_t length = data.size();

  crc = 0x02;
  for (i = 0; i < length; i++)
  {
    crc ^= data[i];
    for (bit = 0; bit < 8; bit++)
    {
      if ((crc & 0x80) != 0)
      {
        crc <<= 1;
        crc ^= 0x7;
      }
      else
      {
        crc <<= 1;
      }
    }
  }

  return crc ^ 0x02;
}

inline uint8_t crc8(u_int8_t *data, int length)
{
  unsigned long crc;
  int i, bit;

  crc = 0x02;
  for (i = 0; i < length; i++)
  {
    crc ^= data[i];
    for (bit = 0; bit < 8; bit++)
    {
      if ((crc & 0x80) != 0)
      {
        crc <<= 1;
        crc ^= 0x7;
      }
      else
      {
        crc <<= 1;
      }
    }
  }

  return crc ^ 0x02;
}

void addCRC(CircularBuffer<uint8_t, BALBOA_MESSAGE_SIZE> &data)
{
  // Add telegram length
  data.unshift(data.size() + 2);

  // Add CRC
  data.push(crc8(data));

  // Wrap telegram in SOF/EOF
  data.unshift(0x7E);
  data.push(0x7E);
}

void rs485Write(CircularBuffer<uint8_t, BALBOA_MESSAGE_SIZE> &data)
{
  // The following is not required for the new RS485 chip
  if (AUTO_TX)
  {
  }
  else
  {
    digitalWrite(TX485_Tx, HIGH);
    delay(1);
  }
  for (int i = 0; i < data.size(); i++)
    RS485_SERIAL_PORT.write(data[i]);

  RS485_SERIAL_PORT.flush();
  rs485LedNotifyTx();

  if (AUTO_TX)
  {
  }
  else
  {
    digitalWrite(TX485_Tx, LOW);
  }

  if (data[4] != Nothing_to_Send_Type)
  {
    Log.verbose(F("[rs485]: Sent: %s" CR), msgToString(data).c_str());
    BRIDGE_LOG_NOISY(F("[BridgeDiag]: rs485_sent ms=%lu frame=%s" CR), millis(), msgToString(data).c_str());
  }
  data.clear();
}

void applyRs485Polarity(bool inverted)
{
  rs485PolarityInverted = inverted;
  rs485Stats.polarityInverted = rs485PolarityInverted ? 1 : 0;

  RS485_SERIAL_PORT.end();
  RS485_SERIAL_PORT.begin(RS485_BAUD_RATE, SERIAL_8N1, TX485_Rx, TX485_Tx);
#if defined(ARDUINO_ARCH_ESP32)
  RS485_SERIAL_PORT.setRxInvert(rs485PolarityInverted);
  if (rs485PolarityInverted)
  {
    uart_set_line_inverse(RS485_UART_NUM, static_cast<uart_signal_inv_t>(UART_SIGNAL_RXD_INV | UART_SIGNAL_TXD_INV));
  }
  else
  {
    uart_set_line_inverse(RS485_UART_NUM, UART_SIGNAL_INV_DISABLE);
  }
#endif
  rs485PolarityDetectWindowStartMs = millis();

  Log.notice(F("[rs485]: UART mode set to %s (RX/TX %s)" CR),
             rs485ModeName(rs485PolarityInverted),
             rs485PolarityInverted ? "inverted" : "normal");
}

const char *rs485ModeName(bool inverted)
{
  return inverted ? "inverted_rx_tx" : "normal";
}

const char *rs485HealthCode()
{
  if (rs485Stats.rawBytesToday == 0 && rs485Stats.lastByteMs == 0)
  {
    return "NO_UART_BYTES";
  }
  if (rs485Stats.messagesToday == 0)
  {
    return "UART_BYTES_NO_VALID_FRAMES";
  }
  return "VALID_FRAMES_OK";
}

int rs485RxGpio()
{
  return TX485_Rx;
}

int rs485TxGpio()
{
  return TX485_Tx;
}

int rs485Baud()
{
  return RS485_BAUD_RATE;
}

bool rs485AutoTxEnabled()
{
  return AUTO_TX;
}

void rs485RecordRawByte(uint8_t value, uint8_t uartAvailable)
{
  const uint32_t nowMs = millis();
  uint32_t gap = 0;
  if (rs485LastRawCaptureMs != 0)
  {
    gap = nowMs - rs485LastRawCaptureMs;
    if (gap > 65535)
    {
      gap = 65535;
    }
  }
  rs485LastRawCaptureMs = nowMs;

  Rs485RawByte b = {};
  b.tMs = nowMs;
  b.gapMs = static_cast<uint16_t>(gap);
  b.value = value;
  b.polarityInverted = rs485PolarityInverted ? 1 : 0;
  b.uartAvailable = uartAvailable;

  if (rs485RawCaptureCount == RS485_RAW_CAPTURE_SIZE)
  {
    rs485Stats.rawCaptureOverflowsToday++;
  }
  rs485RawCapture[rs485RawCaptureHead] = b;
  rs485RawCaptureHead = (rs485RawCaptureHead + 1) % RS485_RAW_CAPTURE_SIZE;
  if (rs485RawCaptureCount < RS485_RAW_CAPTURE_SIZE)
  {
    rs485RawCaptureCount++;
  }
}

void rs485SampleHistory()
{
  const uint32_t nowMs = millis();
  if (rs485LastSampleMs != 0 && nowMs - rs485LastSampleMs < 5000)
  {
    return;
  }
  rs485LastSampleMs = nowMs;

  Rs485Snapshot s = {};
  s.tMs = nowMs;
  s.rawBytesToday = rs485Stats.rawBytesToday;
  s.rawBytesNormalToday = rs485Stats.rawBytesNormalToday;
  s.rawBytesInvertedToday = rs485Stats.rawBytesInvertedToday;
  s.framesToday = rs485Stats.framesToday;
  s.messagesToday = rs485Stats.messagesToday;
  s.crcToday = rs485Stats.crcToday;
  s.badFormatToday = rs485Stats.badFormatToday;
  s.polaritySwitchesToday = rs485Stats.polaritySwitchesToday;
  s.polarityInverted = rs485Stats.polarityInverted;
  s.polarityLocked = rs485Stats.polarityLocked;
  s.detectPhase = rs485Stats.polarityLocked ? 2 : (rs485Stats.polarityInverted ? 1 : 0);
  strncpy(s.health, rs485HealthCode(), sizeof(s.health) - 1);

  rs485History[rs485HistoryHead] = s;
  rs485HistoryHead = (rs485HistoryHead + 1) % RS485_HISTORY_SIZE;
  if (rs485HistoryCount < RS485_HISTORY_SIZE)
  {
    rs485HistoryCount++;
  }
}

int rs485GetHistoryNewestFirst(Rs485Snapshot *out, int maxCount)
{
  if (out == nullptr || maxCount <= 0 || rs485HistoryCount == 0)
  {
    return 0;
  }
  const int toCopy = (maxCount < rs485HistoryCount) ? maxCount : rs485HistoryCount;
  for (int i = 0; i < toCopy; i++)
  {
    int idx = static_cast<int>(rs485HistoryHead) - 1 - i;
    while (idx < 0)
    {
      idx += RS485_HISTORY_SIZE;
    }
    out[i] = rs485History[idx];
  }
  return toCopy;
}

int rs485GetRawRecent(Rs485RawByte *out, int maxCount)
{
  if (out == nullptr || maxCount <= 0 || rs485RawCaptureCount == 0)
  {
    return 0;
  }
  const int toCopy = (maxCount < rs485RawCaptureCount) ? maxCount : rs485RawCaptureCount;
  int idx = static_cast<int>(rs485RawCaptureHead) - toCopy;
  while (idx < 0)
  {
    idx += RS485_RAW_CAPTURE_SIZE;
  }
  for (int i = 0; i < toCopy; i++)
  {
    out[i] = rs485RawCapture[idx];
    idx = (idx + 1) % RS485_RAW_CAPTURE_SIZE;
  }
  return toCopy;
}

bool isMessageValid(CircularBuffer<uint8_t, BALBOA_MESSAGE_SIZE> &data)
{
  if (data.size() < 2)
  {
    return false;
  }
  if (data[0] != 0x7E)
  {
    return false;
  }
  if (data[1] != data.size() - 2)
  {
    return false;
  }
  if (data.size() > BALBOA_MESSAGE_SIZE)
  {
    return false;
  }
  uint8_t message[BALBOA_MESSAGE_SIZE] = {0};
  for (int i = 1; i < data.size() - 2; i++)
  {
    message[i - 1] = data[i];
  }
  //  Log.verbose(F("[rs485]: Data: %d - %s" CR), data.size(), msgToString(data).c_str());
  //  Log.verbose(F("[rs485]: message: %s" CR), msgToString(message, data.size() - 3).c_str());
  //  Log.verbose(F("[rs485]: CRC: %x, %x" CR), crc8(message, data.size() - 3), data[data[1]]);
  if (crc8(message, data.size() - 3) != data[data[1]])
  {
    rs485Stats.crcToday++;
    return false;
  }
  return true;
} // message[message[1]]

void sendExistingClientResponse(uint8_t id)
{
  CircularBuffer<uint8_t, BALBOA_MESSAGE_SIZE> dataBuffer;
  dataBuffer.push(id);
  dataBuffer.push(0xBF);
  dataBuffer.push(0x05);
  dataBuffer.push(0x04);
  dataBuffer.push(0x37);
  dataBuffer.push(0x00); // 08 10 BF 05 04 08 00 - Config request doesn't seem to work

  addCRC(dataBuffer);
  rs485Write(dataBuffer);
  Log.verbose(F("[rs485]: Sent Existing Client Response" CR), msgToString(dataBuffer).c_str());
}

/*
bool hasDayChanged() {
  time_t currentTime = now();  // Get the current time
  if (hour(currentTime) == 0 && hour(lastCheckedTime) != 0) {
    lastCheckedTime = currentTime;  // Update last checked time
    return true;  // Day has changed
  }
  lastCheckedTime = currentTime;  // Update last checked time
  return false;  // No day change
}
*/
