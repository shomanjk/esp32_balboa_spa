#ifndef FAULT_CAPTURE_H
#define FAULT_CAPTURE_H

#include <Arduino.h>

/** Ring of fault entries in RTC slow memory (survives panic reboot; not power loss). */
void faultCaptureInit();
/** Call once early in boot; appends a line if last reset was panic/WDT. */
void faultCaptureOnBootFromResetReason();
/** Append one line (truncated); safe from multiple contexts — keep short. */
void faultCaptureAppend(const char *line);
/** printf-style append into internal buffer (truncated). */
void faultCaptureAppendf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#include <ArduinoJson.h>
/**
 * Append oldest→newest faults into `root["faultLog"]` as objects:
 * `{ uptimeMs, msg, wallUnix?, wallTime? }`, plus `deviceUptimeMs` for same-boot comparison.
 * Without DIAG_FAULT_CAPTURE: empty `faultLog`, empty `lastBridgeIngress`, and `deviceUptimeMs`.
 */
void faultCaptureAppendToJson(JsonObject root);

#if defined(DIAG_FAULT_CAPTURE)
/**
 * Called from linker-wrapped `esp_system_abort` before the system panics.
 * Uses only RTC writes (no heap); safe for many abort/assert paths.
 */
void faultCaptureRecordEspSystemAbort(const char *details);
/** Overwrites RTC copy of last TCP/4257 ingress frame (ASCII hex), for post-mortem without serial. */
void faultCaptureSetLastBridgeIngress(const char *asciiFrame);
#endif

#endif
