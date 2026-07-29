#include "wifiModule.h"
#include <WiFi.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <ArduinoLog.h>
#include <esp_task_wdt.h>
#include <TelnetStream.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#include <spaCommunication.h>
#include <restartReason.h>

WiFiManager wifiManager;
char gatewayName[20];
static uint8_t lastOtaProgressLoggedPercent = 0;
static unsigned long wifiOfflineSinceMs = 0;
static unsigned long wifiOfflineLastLogMs = 0;
static bool wifiOfflineRestartArmed = true;

static bool wifiOtaStarted = false;
static bool wifiTelnetStarted = false;
static bool wifiNtpConfigured = false;
static bool wifiTimeLogged = false;

static bool wifiConnectInFlight = false;
static unsigned long wifiConnectAttemptStartedMs = 0;
static unsigned long wifiNextAttemptDueMs = 0;
static unsigned long wifiReconnectBackoffMs = WIFI_RECONNECT_INITIAL_MS;
static unsigned long wifiConnectAttempts = 0;
static bool wifiBootConnectPending = true;
static bool wifiSuppressDisconnectBackoff = false;

static volatile bool wifiGotIpPending = false;
static volatile bool wifiDisconnectPending = false;
static volatile uint8_t wifiLastDisconnectReason = 0;

static bool wifiBssidLock = false;
static uint8_t wifiBssidBytes[6] = {0};
static char wifiBssidLockStr[18] = "";

static const __FlashStringHelper *otaErrorString(ota_error_t error)
{
  switch (error)
  {
  case OTA_AUTH_ERROR:
    return F("Auth Failed");
  case OTA_BEGIN_ERROR:
    return F("Begin Failed");
  case OTA_CONNECT_ERROR:
    return F("Connect Failed");
  case OTA_RECEIVE_ERROR:
    return F("Receive Failed");
  case OTA_END_ERROR:
    return F("End Failed");
  default:
    return F("Unknown OTA Error");
  }
}

static void formatMac6(const uint8_t *mac, char *out, size_t outLen)
{
  if (!out || outLen < 18)
  {
    return;
  }
  snprintf(out, outLen, "%02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool parseMac6(const char *s, uint8_t out[6])
{
  if (!s || !out)
  {
    return false;
  }
  // Exact "xx:xx:xx:xx:xx:xx" or "xx-xx-xx-xx-xx-xx" (17 chars, no trailing junk).
  if (strlen(s) != 17)
  {
    return false;
  }
  const char sep = s[2];
  if (sep != ':' && sep != '-')
  {
    return false;
  }
  for (int i = 0; i < 6; i++)
  {
    const char *p = s + (i * 3);
    if (i < 5 && p[2] != sep)
    {
      return false;
    }
    if (!isxdigit(static_cast<unsigned char>(p[0])) || !isxdigit(static_cast<unsigned char>(p[1])))
    {
      return false;
    }
    char hex[3] = {p[0], p[1], '\0'};
    out[i] = static_cast<uint8_t>(strtoul(hex, nullptr, 16));
  }
  return true;
}

static void scheduleReconnectBackoff(unsigned long nowMs)
{
  wifiNextAttemptDueMs = nowMs + wifiReconnectBackoffMs;
  unsigned long next = wifiReconnectBackoffMs;
  if (next < 15000UL)
  {
    next = 15000UL;
  }
  else if (next < 30000UL)
  {
    next = 30000UL;
  }
  else
  {
    next = WIFI_RECONNECT_MAX_MS;
  }
  if (next > WIFI_RECONNECT_MAX_MS)
  {
    next = WIFI_RECONNECT_MAX_MS;
  }
  wifiReconnectBackoffMs = next;
}

static void resetReconnectBackoff()
{
  wifiReconnectBackoffMs = WIFI_RECONNECT_INITIAL_MS;
}

static void armOfflineWatchdogIfNeeded(unsigned long nowMs)
{
  if (wifiOfflineSinceMs == 0)
  {
    wifiOfflineSinceMs = nowMs;
    wifiOfflineLastLogMs = 0;
    Log.warning(F("[WiFi]: Offline watchdog started at %lums" CR), wifiOfflineSinceMs);
  }
}

static void clearOfflineWatchdogOnRecovery()
{
  if (wifiOfflineSinceMs != 0)
  {
    unsigned long recoveredMs = millis() - wifiOfflineSinceMs;
    Log.notice(F("[WiFi]: Connectivity restored after %lums offline" CR), recoveredMs);
  }
  wifiOfflineSinceMs = 0;
  wifiOfflineLastLogMs = 0;
  wifiOfflineRestartArmed = true;
}

static void onWifiArduinoEvent(WiFiEvent_t event, WiFiEventInfo_t info)
{
  switch (event)
  {
  case ARDUINO_EVENT_WIFI_STA_GOT_IP:
    wifiGotIpPending = true;
    break;
  case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
  {
    uint8_t reason = info.wifi_sta_disconnected.reason;
    if (reason == 0)
    {
      reason = 1; // WIFI_REASON_UNSPECIFIED
    }
    wifiLastDisconnectReason = reason;
    wifiDisconnectPending = true;
    break;
  }
  default:
    break;
  }
}

static void applyConnectedSideEffects()
{
  if (!wifiNtpConfigured)
  {
    configTime(gmtOffset_sec, daylightOffset_sec, "pool.ntp.org");
    wifiNtpConfigured = true;
  }

  Log.notice(F("[WiFi]: Connected, IP Address: %s" CR), WiFi.localIP().toString().c_str());
  if (wifiBssidLock)
  {
    Log.notice(F("[WiFi]: AP BSSID %s (locked)" CR), WiFi.BSSIDstr().c_str());
  }
  else
  {
    Log.notice(F("[WiFi]: AP BSSID %s (strongest-AP select)" CR), WiFi.BSSIDstr().c_str());
  }

  if (!wifiTimeLogged)
  {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 0))
    {
      char timeCharArray[64];
      strftime(timeCharArray, sizeof(timeCharArray), "%Y-%m-%d %H:%M:%S", &timeinfo);
      Log.notice(F("[WiFi]: Time: %s" CR), timeCharArray);
      wifiTimeLogged = true;
    }
    else
    {
      Log.notice(F("[WiFi]: Time: pending NTP" CR));
    }
  }

  if (!wifiOtaStarted)
  {
    otaSetup();
    wifiOtaStarted = true;
  }

#ifdef TELNET_LOG
  if (!wifiTelnetStarted)
  {
    Log.notice(F("[WiFi]: TelnetStream listening on TCP port 23 (IP %p)" CR), WiFi.localIP());
    TelnetStream.begin();
    Log.notice(F("[WiFi]: Telnet listener up; serial + web log tee unchanged" CR));
    wifiTelnetStarted = true;
  }
#endif
}

static void maybeLogPendingTime()
{
  if (wifiTimeLogged || WiFi.status() != WL_CONNECTED)
  {
    return;
  }
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 0))
  {
    char timeCharArray[64];
    strftime(timeCharArray, sizeof(timeCharArray), "%Y-%m-%d %H:%M:%S", &timeinfo);
    Log.notice(F("[WiFi]: Time: %s" CR), timeCharArray);
    wifiTimeLogged = true;
  }
}

void wifiModuleSetup()
{
  String s = WiFi.macAddress();
  sprintf(gatewayName, "spa-%.2s%.2s%.2s%.2s%.2s%.2s", s.c_str(),
          s.c_str() + 3, s.c_str() + 6, s.c_str() + 9, s.c_str() + 12,
          s.c_str() + 15);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(false);
  WiFi.setHostname(gatewayName);
  ArduinoOTA.setHostname(gatewayName);
  // TX power after STA is up — setTxPower can no-op before mode start on this core.
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

#ifdef WIFI_BSSID
  if (parseMac6(WIFI_BSSID, wifiBssidBytes))
  {
    wifiBssidLock = true;
    formatMac6(wifiBssidBytes, wifiBssidLockStr, sizeof(wifiBssidLockStr));
    Log.notice(F("[WiFi]: BSSID lock enabled: %s" CR), wifiBssidLockStr);
  }
  else
  {
    wifiBssidLock = false;
    wifiBssidLockStr[0] = '\0';
    Log.error(F("[WiFi]: Invalid WIFI_BSSID \"%s\" — falling back to SSID-only / strongest-AP" CR), WIFI_BSSID);
  }
#else
  wifiBssidLock = false;
  wifiBssidLockStr[0] = '\0';
#endif

  if (!wifiBssidLock)
  {
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    Log.notice(F("[WiFi]: Strongest-AP scan/sort enabled (no BSSID lock)" CR));
  }

  WiFi.onEvent(onWifiArduinoEvent);

  wifiBootConnectPending = true;
  wifiNextAttemptDueMs = 0;
  resetReconnectBackoff();

  Log.notice(F("[WiFi]: Hostname: %s" CR), WiFi.getHostname());
  Log.notice(F("[WiFi]: OTA Hostname: %s" CR), ArduinoOTA.getHostname().c_str());
  Log.notice(F("[WiFi]: App-owned async reconnect (autoReconnect=false)" CR));
}

void wifiConnect()
{
  if (wifiConnectInFlight || WiFi.status() == WL_CONNECTED)
  {
    return;
  }

  wifiConnectAttempts++;
  wifiConnectInFlight = true;
  wifiConnectAttemptStartedMs = millis();

  if (wifiBssidLock)
  {
    Log.notice(F("[WiFi]: Connecting to %s (BSSID %s) attempt #%lu" CR),
               WIFI_SSID, wifiBssidLockStr, wifiConnectAttempts);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD, 0, wifiBssidBytes, true);
  }
  else
  {
    Log.notice(F("[WiFi]: Connecting to %s (strongest AP) attempt #%lu" CR),
               WIFI_SSID, wifiConnectAttempts);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

static void failConnectAttempt(unsigned long nowMs, const __FlashStringHelper *why)
{
  if (!wifiConnectInFlight)
  {
    return;
  }
  wifiConnectInFlight = false;
  // Abort any stale ESP association before the next app-owned begin().
  // Suppress backoff from the resulting DISCONNECTED event — we schedule once here.
  // If disconnect fails / no event is expected, clear suppress immediately so the
  // next real disconnect does not skip backoff.
  wifiSuppressDisconnectBackoff = true;
  if (!WiFi.disconnect(false))
  {
    wifiSuppressDisconnectBackoff = false;
  }
  Log.warning(F("[WiFi]: Connect attempt failed (%s), next in %lums" CR), why, wifiReconnectBackoffMs);
  scheduleReconnectBackoff(nowMs);
}

void wifiModuleLoop()
{
  const unsigned long nowMs = millis();

  if (wifiDisconnectPending)
  {
    wifiDisconnectPending = false;
    wifiConnectInFlight = false;
    const uint8_t reason = wifiLastDisconnectReason;
    const char *reasonName = WiFi.disconnectReasonName(static_cast<wifi_err_reason_t>(reason));
    Log.warning(F("[WiFi]: Disconnected reason=%u (%s)" CR), reason, reasonName ? reasonName : "?");
    armOfflineWatchdogIfNeeded(nowMs);
    if (wifiSuppressDisconnectBackoff)
    {
      wifiSuppressDisconnectBackoff = false;
    }
    else if (WiFi.status() != WL_CONNECTED)
    {
      scheduleReconnectBackoff(nowMs);
    }
  }

  if (wifiGotIpPending)
  {
    wifiGotIpPending = false;
    wifiConnectInFlight = false;
    wifiBootConnectPending = false;
    resetReconnectBackoff();
    clearOfflineWatchdogOnRecovery();
    applyConnectedSideEffects();
  }

  maybeLogPendingTime();

  if (WiFi.status() == WL_CONNECTED)
  {
    ArduinoOTA.handle();
#ifdef TELNET_LOG
    switch (TelnetStream.read())
    {
    case 'C':
      TelnetStream.println("bye bye");
      TelnetStream.flush();
      TelnetStream.stop();
      break;
    }
#endif
    return;
  }

  // Offline path — watchdog continuous across retries (do not reset wifiOfflineSinceMs here).
  armOfflineWatchdogIfNeeded(nowMs);
  unsigned long offlineMs = nowMs - wifiOfflineSinceMs;
  if (offlineMs >= WIFI_OFFLINE_RESTART_LOG_INTERVAL_MS &&
      (wifiOfflineLastLogMs == 0 || (nowMs - wifiOfflineLastLogMs) >= WIFI_OFFLINE_RESTART_LOG_INTERVAL_MS))
  {
    wifiOfflineLastLogMs = nowMs;
    Log.warning(F("[WiFi]: Offline for %lums (restart at %lums)" CR), offlineMs, (unsigned long)WIFI_OFFLINE_RESTART_TIMEOUT_MS);
  }
  if (wifiOfflineRestartArmed &&
      offlineMs >= WIFI_OFFLINE_RESTART_TIMEOUT_MS &&
      nowMs >= WIFI_OFFLINE_RESTART_MIN_UPTIME_MS)
  {
    wifiOfflineRestartArmed = false;
    setLastRestartReason("WiFi offline watchdog");
    Log.error(F("[WiFi]: Offline timeout reached (%lums), restarting" CR), offlineMs);
    delay(50);
    ESP.restart();
    return;
  }

  if (wifiConnectInFlight)
  {
    if ((nowMs - wifiConnectAttemptStartedMs) >= WIFI_CONNECT_ATTEMPT_TIMEOUT_MS)
    {
      failConnectAttempt(nowMs, F("attempt timeout"));
    }
    return;
  }

  // Wrap-safe: unsigned elapsed comparison survives millis() rollover (~49.7 days).
  const bool due = wifiBootConnectPending ||
                   ((int32_t)(nowMs - wifiNextAttemptDueMs) >= 0);
  if (due)
  {
    wifiBootConnectPending = false;
    wifiConnect();
  }
}

String getStringTime()
{
  struct tm timeinfo;
  // Non-blocking by default so callers cannot stall RS485 loop after GOT_IP.
  if (!getLocalTime(&timeinfo, 0))
  {
    return String("Time pending");
  }
  char timeCharArray[64];
  strftime(timeCharArray, sizeof(timeCharArray), "%Y-%m-%d %H:%M:%S", &timeinfo);

  return String(timeCharArray);
}

void otaSetup()
{
  if (ENABLE_OTA_AUTH)
  {
    if (String(OTA_PASSWORD).length() == 0)
    {
      Log.warning(F("[WiFi]: OTA auth enabled but OTA_PASSWORD is empty" CR));
    }
    ArduinoOTA.setPassword(OTA_PASSWORD);
    Log.notice(F("[WiFi]: OTA auth enabled (trusted LAN override disabled)" CR));
  }
  else
  {
    Log.warning(F("[WiFi]: OTA auth disabled; trusted LAN mode active" CR));
  }
  ArduinoOTA.setTimeout(OTA_TIMEOUT_MS);
  ArduinoOTA.onStart(notifyOfUpdateStarted);
  ArduinoOTA.onEnd(notifyOfUpdateEnded);
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
                        {
    if (total == 0)
    {
      return;
    }
    uint8_t progressPercent = (progress * 100) / total;
    if (progressPercent >= lastOtaProgressLoggedPercent + OTA_PROGRESS_LOG_STEP_PERCENT || progressPercent == 100)
    {
      lastOtaProgressLoggedPercent = progressPercent;
      Log.notice(F("[WiFi]: OTA Progress: %u%%" CR), progressPercent);
    }
    esp_task_wdt_reset(); });
  ArduinoOTA.onError([](ota_error_t error)
                     {
    Log.error(F("[WiFi]: OTA Error: %s (%d)" CR), otaErrorString(error), error);
    setLastRestartReason("OTA error"); });
  ArduinoOTA.begin();
  Log.notice(F("[WiFi]: Arduino OTA Enabled" CR));
}

void notifyOfUpdateStarted()
{
  lastOtaProgressLoggedPercent = 0;
  Log.notice(F("[WiFi]: Arduino OTA Update Start" CR));
  setLastRestartReason("OTA start");
  spaCommunicationEnd();
}

void notifyOfUpdateEnded()
{
  Log.notice(F("[WiFi]: Arduino OTA Update Complete" CR));
  setLastRestartReason("OTA Update");
}

bool wifiBssidLockActive()
{
  return wifiBssidLock;
}

const char *wifiConfiguredBssidLock()
{
  return wifiBssidLockStr;
}

uint8_t wifiLastDisconnectReasonCode()
{
  return wifiLastDisconnectReason;
}

unsigned long wifiConnectAttemptCount()
{
  return wifiConnectAttempts;
}
