#include "wifiModule.h"
#include <WiFi.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <ArduinoLog.h>
#include <esp_task_wdt.h>
#include <TelnetStream.h>
#include <time.h>

#include <spaCommunication.h>
#include <restartReason.h>

WiFiManager wifiManager;
char gatewayName[20];
static uint8_t lastOtaProgressLoggedPercent = 0;
static unsigned long wifiOfflineSinceMs = 0;
static unsigned long wifiOfflineLastLogMs = 0;
static bool wifiOfflineRestartArmed = true;

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

void wifiModuleSetup()
{
  String s = WiFi.macAddress();
  sprintf(gatewayName, "spa-%.2s%.2s%.2s%.2s%.2s%.2s", s.c_str(),
          s.c_str() + 3, s.c_str() + 6, s.c_str() + 9, s.c_str() + 12,
          s.c_str() + 15);

  WiFi.setTxPower(WIFI_POWER_19_5dBm); // this sets wifi to highest power
  WiFi.setSleep(false);
  WiFi.setHostname(gatewayName);
  ArduinoOTA.setHostname(gatewayName);
  Log.notice(F("[WiFi]: Hostname: %s" CR), WiFi.getHostname());
  Log.notice(F("[WiFi]: OTA Hostname: %s" CR), ArduinoOTA.getHostname().c_str());
}

void wifiModuleLoop()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    if (wifiOfflineSinceMs == 0)
    {
      wifiOfflineSinceMs = millis();
      wifiOfflineLastLogMs = 0;
      Log.warning(F("[WiFi]: Offline watchdog started at %lums" CR), wifiOfflineSinceMs);
    }
    unsigned long nowMs = millis();
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
    wifiConnect();
  }
  else
  {
    if (wifiOfflineSinceMs != 0)
    {
      unsigned long recoveredMs = millis() - wifiOfflineSinceMs;
      Log.notice(F("[WiFi]: Connectivity restored after %lums offline" CR), recoveredMs);
      wifiOfflineSinceMs = 0;
      wifiOfflineLastLogMs = 0;
      wifiOfflineRestartArmed = true;
    }
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
  }
}

/* Functions */

void wifiConnect()
{
  Log.notice(F("[WiFi]: Connecting to %s" CR), WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long timeout = millis() + WIFI_CONNECT_TIMEOUT;

  while (WiFi.status() != WL_CONNECTED && millis() < timeout)
  {
    yield();
  }
  if (WiFi.status() != WL_CONNECTED)
  {
    Log.error(F("[WiFi]: Connect failed to %s" CR), WIFI_SSID);
  }
  else
  {
    configTime(gmtOffset_sec, daylightOffset_sec, "pool.ntp.org");
    Log.notice(F("[WiFi]: Connected, IP Address: %s" CR), WiFi.localIP().toString().c_str());
    Log.notice(F("[WiFi]: Time: %s" CR), getStringTime().c_str());
    otaSetup();
#ifdef TELNET_LOG
    Log.notice(F("[WiFi]: TelnetStream listening on TCP port 23 (IP %p)" CR), WiFi.localIP());
    TelnetStream.begin();
    // Keep the global logger on Serial. Switching all logs to TelnetStream can
    // stall output paths and trigger WDT resets on some runtimes.
    Log.notice(F("[WiFi]: Telnet listener up; serial + web log tee unchanged" CR));
#endif
  }
}

String getStringTime()
{
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
  {
    Log.error(F("[WiFi]: Obtaining Time failed" CR));
    return String("Failed to obtain time");
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