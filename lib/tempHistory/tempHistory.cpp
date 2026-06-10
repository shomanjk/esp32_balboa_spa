#include <LittleFS.h>
#include <tempHistory.h>
#include <spaUtilities.h>
#include <ArduinoLog.h>
#include <cstring>

static const char kTempHistoryFile[] = "/TempHist.bin";

typedef struct __attribute__((packed))
{
  uint16_t storeVersion;
  uint16_t slotCount;
  uint32_t magicNumber;
  time_t lastSampleUnix;
  float samples[TEMP_HISTORY_SLOTS];
} TempHistoryStoreV1;

static void tempHistoryReset(TempHistoryData *data)
{
  data->magicNumber = TEMP_HISTORY_MAGIC_NUMBER;
  data->lastPersistMs = millis();
  data->lastSampleUnix = 0;
  for (int i = 0; i < TEMP_HISTORY_SLOTS; i++)
  {
    data->samples[i] = 0.0f;
    data->lastPersistedSamples[i] = 0.0f;
  }
}

static bool tempHistoryLoadFromFlash(TempHistoryData *data)
{
  File file = LittleFS.open(kTempHistoryFile, "r");
  if (!file)
  {
    Log.warning(F("[TempHist]: No saved data at %s" CR), kTempHistoryFile);
    return false;
  }

  TempHistoryStoreV1 store = {};
  const size_t need = sizeof(TempHistoryStoreV1);
  if (file.readBytes(reinterpret_cast<char *>(&store), need) != need)
  {
    Log.error(F("[TempHist]: Failed to read %s" CR), kTempHistoryFile);
    file.close();
    return false;
  }
  file.close();

  if (store.storeVersion != TEMP_HISTORY_STORE_VERSION || store.slotCount != TEMP_HISTORY_SLOTS ||
      store.magicNumber != TEMP_HISTORY_MAGIC_NUMBER)
  {
    Log.warning(F("[TempHist]: Unsupported or invalid store in %s (ver=%u slots=%u)" CR),
                kTempHistoryFile, static_cast<unsigned>(store.storeVersion),
                static_cast<unsigned>(store.slotCount));
    return false;
  }

  data->magicNumber = store.magicNumber;
  data->lastSampleUnix = store.lastSampleUnix;
  memcpy(data->samples, store.samples, sizeof(data->samples));
  memcpy(data->lastPersistedSamples, store.samples, sizeof(data->lastPersistedSamples));
  data->lastPersistMs = millis();
  Log.notice(F("[TempHist]: Loaded %u samples from %s" CR), static_cast<unsigned>(TEMP_HISTORY_SLOTS),
             kTempHistoryFile);
  return true;
}

static void tempHistorySaveToFlash(const TempHistoryData *data)
{
  TempHistoryStoreV1 store = {};
  store.storeVersion = TEMP_HISTORY_STORE_VERSION;
  store.slotCount = TEMP_HISTORY_SLOTS;
  store.magicNumber = data->magicNumber;
  store.lastSampleUnix = data->lastSampleUnix;
  memcpy(store.samples, data->samples, sizeof(store.samples));

  File file = LittleFS.open(kTempHistoryFile, "w");
  if (!file)
  {
    Log.error(F("[TempHist]: Failed to open %s for writing" CR), kTempHistoryFile);
    return;
  }

  if (file.write(reinterpret_cast<const uint8_t *>(&store), sizeof(store)) != sizeof(store))
  {
    Log.error(F("[TempHist]: Failed to write %s" CR), kTempHistoryFile);
  }
  else
  {
    Log.notice(F("[TempHist]: Saved %u samples to %s" CR), static_cast<unsigned>(TEMP_HISTORY_SLOTS),
               kTempHistoryFile);
  }
  file.close();
}

void tempHistorySetup(TempHistoryData *data)
{
  if (data == nullptr)
  {
    return;
  }

  if (data->magicNumber != TEMP_HISTORY_MAGIC_NUMBER)
  {
    if (!tempHistoryLoadFromFlash(data))
    {
      tempHistoryReset(data);
    }
  }
  else
  {
    Log.notice(F("[TempHist]: Using RTC temperature history buffer" CR));
    data->lastPersistMs = millis();
  }
}

void tempHistorySample(TempHistoryData *data, float temp)
{
  if (data == nullptr)
  {
    return;
  }

  for (int x = TEMP_HISTORY_SLOTS; x > 1; x--)
  {
    data->samples[x - 1] = data->samples[x - 2];
  }
  data->samples[0] = temp;
  data->lastSampleUnix = getTime();
}

void tempHistoryMaybePersist(TempHistoryData *data)
{
  if (data == nullptr)
  {
    return;
  }

  const unsigned long nowMs = millis();
  if (nowMs - data->lastPersistMs < TEMP_FLASH_SAVE_MIN_MS)
  {
    return;
  }

  data->lastPersistMs = nowMs;

  if (memcmp(data->samples, data->lastPersistedSamples, sizeof(data->samples)) == 0)
  {
    Log.verbose(F("[TempHist]: Skip flash save (buffer unchanged)" CR));
    return;
  }

  tempHistorySaveToFlash(data);
  memcpy(data->lastPersistedSamples, data->samples, sizeof(data->lastPersistedSamples));
}

const float *tempHistorySamplesNewestFirst(const TempHistoryData *data)
{
  return data != nullptr ? data->samples : nullptr;
}

void tempHistoryCopyOldestFirst(const TempHistoryData *data, float *out, size_t count)
{
  if (data == nullptr || out == nullptr)
  {
    return;
  }

  const size_t n = count < TEMP_HISTORY_SLOTS ? count : TEMP_HISTORY_SLOTS;
  for (size_t i = 0; i < n; i++)
  {
    out[i] = data->samples[TEMP_HISTORY_SLOTS - 1 - i];
  }
}
