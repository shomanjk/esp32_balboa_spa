#ifndef RS485_H
#define RS485_H

#include <Arduino.h>
#include <CircularBuffer.hpp>
#include "../../src/main.h"

/** Balboa wire framing: prepend length, append CRC-8 (per protocol), wrap 0x7E … 0x7E. Same as RS485 transmit path. */
void addCRC(CircularBuffer<uint8_t, BALBOA_MESSAGE_SIZE> &data);

#define RS485_WRITE_QUEUE 10
#define RS485_HISTORY_SIZE 60
#define RS485_RAW_CAPTURE_SIZE 256

void rs485Setup();
/** Idempotent UART begin + polarity after Wi‑Fi/OTA; no-op if safe mode or already begun. */
bool rs485EnsureUartBegun();
bool rs485UartBegun();
/** Milliseconds since UART begin, or 0 if not begun. */
uint32_t rs485UartUptimeMs();
/** Valid CRC frames since this boot (RAM; not RTC day counters). */
extern uint32_t rs485ValidFramesSinceBoot;
bool rs485SafeModeActive();
/** Clear safe mode / streak and set retry-pending (main loop calls ensure). */
void rs485RequestRetry();
bool rs485RetryPending();
uint8_t rs485FaultBootStreak();
bool rs485BeginAttemptedFlag();
const char *rs485SafeModeReason();
/** Clear streak after healthy UART uptime; call from loop. */
void rs485BootSafetyTick();
void rs485Loop();
/** After spa id is assigned: restart if no valid RS485 frame for `RUNNING_WDT_TIMEOUT` seconds. */
void rs485CheckSpaSilenceWatchdog();
void rs485ClearToSend();
void rs485SampleHistory();
const char *rs485HealthCode();
int rs485RxGpio();
int rs485TxGpio();
int rs485Baud();
bool rs485AutoTxEnabled();
uint32_t rs485LastCtsMs();
uint32_t rs485CtsCount();
uint32_t rs485NextCtsArmCount();
uint32_t rs485NextCtsFireCount();
bool rs485ArmFrameOnNextCts(const uint8_t *frame, int length, uint32_t *outArmCount = nullptr);

// void rs485Send(uint8_t *data, int length, boolean addCrc, boolean force = false);
// void rs485Send(CircularBuffer<uint8_t, BALBOA_MESSAGE_SIZE> &data, boolean addCrc, boolean force = false);

extern uint8_t id; // spa id

// Analytics

struct Rs485Stats
{
  uint32_t rawBytesToday;
  uint32_t rawBytesYesterday;
  uint32_t rawBytesNormalToday;
  uint32_t rawBytesNormalYesterday;
  uint32_t rawBytesInvertedToday;
  uint32_t rawBytesInvertedYesterday;
  uint32_t framesToday;
  uint32_t framesYesterday;
  uint32_t messagesToday;
  uint32_t messagesYesterday;
  uint32_t badFormatToday;
  uint32_t badFormatYesterday;
  uint32_t crcToday;
  uint32_t crcYesterday;
  uint32_t lastByteMs;
  uint32_t lastValidFrameMs;
  uint32_t polaritySwitchesToday;
  uint32_t polaritySwitchesYesterday;
  uint8_t polarityInverted;
  uint8_t polarityLocked;
  uint32_t frameMarkersToday;
  uint32_t frameMarkersYesterday;
  uint32_t maxUartAvailableToday;
  uint32_t maxUartAvailableYesterday;
  uint32_t rawCaptureOverflowsToday;
  uint32_t rawCaptureOverflowsYesterday;
  uint32_t magicNumber;
};

extern Rs485Stats rs485Stats;

struct Rs485Snapshot
{
  uint32_t tMs;
  uint32_t rawBytesToday;
  uint32_t rawBytesNormalToday;
  uint32_t rawBytesInvertedToday;
  uint32_t framesToday;
  uint32_t messagesToday;
  uint32_t crcToday;
  uint32_t badFormatToday;
  uint32_t polaritySwitchesToday;
  uint8_t polarityInverted;
  uint8_t polarityLocked;
  uint8_t detectPhase;
  char health[32];
};

int rs485GetHistoryNewestFirst(Rs485Snapshot *out, int maxCount);

struct Rs485RawByte
{
  uint32_t tMs;
  uint16_t gapMs;
  uint8_t value;
  uint8_t polarityInverted;
  uint8_t uartAvailable;
};

int rs485GetRawRecent(Rs485RawByte *out, int maxCount);

struct rs485WriteQueueMessage
{
  char message[BALBOA_MESSAGE_SIZE];
  int length;
};

#endif
