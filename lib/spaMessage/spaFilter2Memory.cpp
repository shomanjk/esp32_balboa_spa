#include "spaFilter2Memory.h"

#include <ArduinoLog.h>
#include <Preferences.h>
#include <stdio.h>

namespace
{
const char *kNamespace = "spa-filter2";
const char *kKey = "sched";

SpaFilter2Schedule memorySchedule = {0, 0, 0, 0};
bool memoryLoaded = false;
bool memoryValid = false;

bool plausible(const SpaFilter2Schedule &s)
{
  if (s.startHour > 23 || s.startMinute > 59 || s.durHour > 24 || s.durMinute > 59)
  {
    return false;
  }
  if (s.durHour == 24 && s.durMinute != 0)
  {
    return false;
  }
  return s.durHour != 0 || s.durMinute != 0;
}

// ArduinoLog has no %u / width support, so format the text here.
void formatSchedule(const SpaFilter2Schedule &s, char *out, size_t size)
{
  snprintf(out, size, "%02u:%02u for %02u:%02u", (unsigned)s.startHour, (unsigned)s.startMinute, (unsigned)s.durHour,
           (unsigned)s.durMinute);
}

uint32_t pack(const SpaFilter2Schedule &s)
{
  return ((uint32_t)s.startHour << 24) | ((uint32_t)s.startMinute << 16) | ((uint32_t)s.durHour << 8) | s.durMinute;
}

SpaFilter2Schedule unpack(uint32_t v)
{
  SpaFilter2Schedule s;
  s.startHour = (v >> 24) & 0xFF;
  s.startMinute = (v >> 16) & 0xFF;
  s.durHour = (v >> 8) & 0xFF;
  s.durMinute = v & 0xFF;
  return s;
}

void loadOnce()
{
  if (memoryLoaded)
  {
    return;
  }
  memoryLoaded = true;
  Preferences prefs;
  if (!prefs.begin(kNamespace, false))
  {
    Log.warning(F("[Filter2]: flash storage unavailable, schedule is remembered in RAM only" CR));
    return;
  }
  const uint32_t stored = prefs.getUInt(kKey, 0xFFFFFFFFu);
  prefs.end();
  if (stored == 0xFFFFFFFFu)
  {
    return;
  }
  const SpaFilter2Schedule s = unpack(stored);
  if (plausible(s))
  {
    memorySchedule = s;
    memoryValid = true;
    char text[24];
    formatSchedule(s, text, sizeof(text));
    Log.notice(F("[Filter2]: remembered schedule %s" CR), text);
  }
}
} // namespace

void spaFilter2MemoryRemember(const SpaFilter2Schedule &schedule)
{
  if (!plausible(schedule))
  {
    return;
  }
  loadOnce();
  if (memoryValid && memorySchedule.startHour == schedule.startHour && memorySchedule.startMinute == schedule.startMinute &&
      memorySchedule.durHour == schedule.durHour && memorySchedule.durMinute == schedule.durMinute)
  {
    return;
  }
  memorySchedule = schedule;
  memoryValid = true;

  Preferences prefs;
  if (prefs.begin(kNamespace, false))
  {
    prefs.putUInt(kKey, pack(schedule));
    prefs.end();
  }
  char text[24];
  formatSchedule(schedule, text, sizeof(text));
  Log.notice(F("[Filter2]: schedule remembered %s" CR), text);
}

SpaFilter2Schedule spaFilter2MemoryGet(uint8_t filter1Hour, uint8_t filter1Minute,
                                       uint8_t filter1DurHour, uint8_t filter1DurMinute)
{
  loadOnce();
  if (memoryValid)
  {
    return memorySchedule;
  }
  // Default: starts when filter 1 ends, one hour long.
  const uint32_t endMinute = ((uint32_t)filter1Hour * 60u + filter1Minute + (uint32_t)filter1DurHour * 60u + filter1DurMinute) % (24u * 60u);
  SpaFilter2Schedule fallback;
  fallback.startHour = (uint8_t)(endMinute / 60u);
  fallback.startMinute = (uint8_t)(endMinute % 60u);
  fallback.durHour = 1;
  fallback.durMinute = 0;
  return fallback;
}
