#ifndef TEMP_HISTORY_H
#define TEMP_HISTORY_H

#include <Arduino.h>
#include "../../src/main.h"

#define TEMP_HISTORY_MAGIC_NUMBER 0x54484953u
#define TEMP_HISTORY_STORE_VERSION 1

struct TempHistoryData
{
  uint32_t magicNumber;
  float samples[TEMP_HISTORY_SLOTS];
  unsigned long lastPersistMs;
  float lastPersistedSamples[TEMP_HISTORY_SLOTS];
  time_t lastSampleUnix;
};

void tempHistorySetup(TempHistoryData *data);
void tempHistorySample(TempHistoryData *data, float temp);
void tempHistoryMaybePersist(TempHistoryData *data);
const float *tempHistorySamplesNewestFirst(const TempHistoryData *data);
void tempHistoryCopyOldestFirst(const TempHistoryData *data, float *out, size_t count);

#endif
