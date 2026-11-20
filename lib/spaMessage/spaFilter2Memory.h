#pragma once

#include <Arduino.h>

/**
 * Remembered filter 2 schedule.
 *
 * The controller zeroes the filter 2 start and duration whenever filter 2 is switched off, so
 * after that the schedule cannot be read back and cannot be staged on the controller. The gateway
 * therefore remembers the last schedule it saw while filter 2 was on (or that was staged while it
 * was off), in RAM and in flash (ESP32 Preferences, written only when the schedule changes, so it
 * survives power loss). It is used to show sensible values while filter 2 is off and to switch
 * filter 2 back on with its previous schedule.
 */
struct SpaFilter2Schedule
{
  uint8_t startHour;
  uint8_t startMinute;
  uint8_t durHour;
  uint8_t durMinute;
};

/** Remember a schedule (ignored when implausible, e.g. zero duration). Writes flash only on change. */
void spaFilter2MemoryRemember(const SpaFilter2Schedule &schedule);

/**
 * The remembered schedule, or when nothing was remembered yet a default: start right after
 * filter 1 ends, duration 60 minutes. Takes the current filter 1 schedule for that default.
 */
SpaFilter2Schedule spaFilter2MemoryGet(uint8_t filter1Hour, uint8_t filter1Minute,
                                       uint8_t filter1DurHour, uint8_t filter1DurMinute);
