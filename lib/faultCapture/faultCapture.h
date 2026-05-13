#ifndef FAULT_CAPTURE_H
#define FAULT_CAPTURE_H

#include <Arduino.h>

/** Ring of short text lines in RTC slow memory (survives panic reboot; not power loss). */
void faultCaptureInit();
/** Call once early in boot; appends a line if last reset was panic/WDT. */
void faultCaptureOnBootFromResetReason();
/** Append one line (truncated); safe from multiple contexts — keep short. */
void faultCaptureAppend(const char *line);
/** printf-style append into internal buffer (truncated). */
void faultCaptureAppendf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#if defined(DIAG_FAULT_CAPTURE)
#include <ArduinoJson.h>
/** Append oldest→newest lines into `root["faultLog"]` (creates array). */
void faultCaptureAppendToJson(JsonObject root);
#endif

#endif
