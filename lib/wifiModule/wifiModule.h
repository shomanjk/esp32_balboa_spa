#ifndef WIFI_MODULE_H
#define WIFI_MODULE_H
#include <Arduino.h>

#include "../../src/config.h" // Default passwords and SSID

#ifndef WIFI_SSID
#warning "WIFI_SSID not defined, please define in config.h"
#define WIFI_SSID "spa"
#endif

#ifndef WIFI_PASSWORD
#warning "WIFI_PASSWORD not defined, please define in config.h"
#define WIFI_PASSWORD "password"
#endif

// Optional: lock STA to one mesh node (AP MAC). Omit for strongest-AP selection.
// Example: #define WIFI_BSSID "aa:bb:cc:dd:ee:ff"

#ifndef ENABLE_OTA_AUTH
#define ENABLE_OTA_AUTH false
#endif

#ifndef OTA_PASSWORD
#define OTA_PASSWORD ""
#endif

#ifndef OTA_TIMEOUT_MS
#define OTA_TIMEOUT_MS 15000
#endif

#ifndef OTA_PROGRESS_LOG_STEP_PERCENT
#define OTA_PROGRESS_LOG_STEP_PERCENT 10
#endif

#ifndef GMT_OFFSET
#warning "GMT_OFFSET not defined, please define in config.h"
#define GMT_OFFSET -14400
#endif

#ifndef DAYLIGHT_OFFSET
#warning "DAYLIGHT_OFFSET not defined, please define in config.h"
#define DAYLIGHT_OFFSET 0
#endif

// Async in-flight association timeout (not a blocking wait inside loop()).
// If only legacy WIFI_CONNECT_TIMEOUT is set in config.h, it is used as this timeout.
#ifndef WIFI_CONNECT_ATTEMPT_TIMEOUT_MS
#ifdef WIFI_CONNECT_TIMEOUT
#define WIFI_CONNECT_ATTEMPT_TIMEOUT_MS (WIFI_CONNECT_TIMEOUT)
#else
#define WIFI_CONNECT_ATTEMPT_TIMEOUT_MS 15000UL
#endif
#endif

// Legacy alias only — prefer WIFI_CONNECT_ATTEMPT_TIMEOUT_MS. No longer a blocking
// WiFi.begin() wait; kept so older config.h values still apply via the mapping above.
#ifndef WIFI_CONNECT_TIMEOUT
#define WIFI_CONNECT_TIMEOUT WIFI_CONNECT_ATTEMPT_TIMEOUT_MS
#endif

#ifndef WIFI_RECONNECT_INITIAL_MS
#define WIFI_RECONNECT_INITIAL_MS 5000UL
#endif

#ifndef WIFI_RECONNECT_MAX_MS
#define WIFI_RECONNECT_MAX_MS 60000UL
#endif

#ifndef WIFI_OFFLINE_RESTART_TIMEOUT_MS
// If Wi-Fi stays offline this long, force a reboot to recover from stale stack/AP states.
#define WIFI_OFFLINE_RESTART_TIMEOUT_MS (10UL * 60UL * 1000UL)
#endif

#ifndef WIFI_OFFLINE_RESTART_MIN_UPTIME_MS
// Avoid reboot churn while the device is still in early boot/reconnect.
#define WIFI_OFFLINE_RESTART_MIN_UPTIME_MS (2UL * 60UL * 1000UL)
#endif

#ifndef WIFI_OFFLINE_RESTART_LOG_INTERVAL_MS
// Throttle offline watchdog progress logs.
#define WIFI_OFFLINE_RESTART_LOG_INTERVAL_MS 30000UL
#endif

const long gmtOffset_sec = GMT_OFFSET;
const int daylightOffset_sec = DAYLIGHT_OFFSET;
extern char gatewayName[20];

void wifiModuleSetup();
void wifiModuleLoop();
void notifyOfUpdateStarted();
void notifyOfUpdateEnded();
/** Start one async association attempt (no blocking wait). */
void wifiConnect();
void otaSetup();

String getStringTime();

/** True when a valid compile-time WIFI_BSSID lock is active. */
bool wifiBssidLockActive();
/** Configured lock string (lowercase aa:bb:…) or empty if unlocked / invalid. */
const char *wifiConfiguredBssidLock();
/** Last disconnect reason code (0 if none yet). */
uint8_t wifiLastDisconnectReasonCode();
/** App-started connect attempts since boot. */
unsigned long wifiConnectAttemptCount();

#endif
