#include <Arduino.h>
#include <WiFiManager.h> //https://github.com/tzapu/WiFiManager WiFi Configuration Magic
#include <ArduinoLog.h>
#include <esp_task_wdt.h>
#include <TelnetStream.h>

// Local Libraries
#include <restartReason.h>
#include <faultCapture.h>
#include <wifiModule.h>
#include <findSpa.h>
#include <spaCommunication.h>
#include <spaMessage.h>
#include <spaWebServer.h>
#include <spaUtilities.h>
#include <mqttModule.h>
#include <rs485.h>
#include <bridge.h>
#include <spaEpaper.h>
#ifdef M5_STATUS_LED
#include <led_control.h>
#endif

#include "main.h"
#include <webLogBuffer.h>

#ifdef M5_STATUS_LED
void onStationModeGotIP(WiFiEvent_t event, WiFiEventInfo_t info) {
    ledControl.setWifiConnected();
}

void onStationModeDisconnected(WiFiEvent_t event, WiFiEventInfo_t info) {
    ledControl.setWifiDisconnected();
}
#endif

String buildDefinitionString = "";
#define addBuildDefinition(name) buildDefinitionString += #name " ";

void setup()
{
  // Launch serial for debugging purposes
  Serial.begin(SERIAL_BAUD);
  webLogBufferSetup(Serial);
  faultCaptureInit();
  faultCaptureOnBootFromResetReason();
  Log.setPrefix(logPrintPrefix);
  Log.begin(LOG_LEVEL, &webLogBufferGetLogPrint());
  esp_task_wdt_init(INITIAL_WDT_TIMEOUT, true); // enable panic so ESP32 restarts
  esp_task_wdt_add(NULL);                       // add current thread to WDT watch
  logSection("WELCOME TO esp32_balboa_spa");
#ifdef spaEpaper
  logSection("EPaper Setup");
  spaEpaperSetup();
#endif
  logSection("Build Definitions");
  Log.notice(F("Version: %s" CR), VERSION);
  Log.notice(F("Build: %s" CR), BUILD);

#ifdef ESP32S3
  Log.notice(F("Build for ESP32S3" CR));
#else
  Log.notice(F("Build for ESP32" CR));
#endif

#ifdef ARDUINO_ESP32S3_DEV
  Log.notice(F("Build for ARDUINO_ESP32S3_DEV" CR));
#endif

#ifdef LOCAL_CONNECT
  addBuildDefinition("LOCAL_CONNECT");
#endif

#ifdef LOCAL_CLIENT
  addBuildDefinition("LOCAL_CLIENT");
#endif

#ifdef REMOTE_CLIENT
  addBuildDefinition("REMOTE_CLIENT");
#endif

#ifdef TELNET_LOG
  addBuildDefinition("TELNET_LOG");
#endif

#ifdef BRIDGE
  addBuildDefinition("BRIDGE");
#endif

#ifdef spaEpaper
  addBuildDefinition("spaEpaper");
#endif

#ifdef M5_STATUS_LED
  addBuildDefinition("M5_STATUS_LED");
#endif

#ifdef DIAG_FAULT_CAPTURE
  addBuildDefinition("DIAG_FAULT_CAPTURE");
#endif

  Log.notice(F("Build Definitions: %s" CR), buildDefinitionString.c_str());

  logSection("ESP Information");
  Log.notice(F("Last restart reason: %s" CR), getLastRestartReason().c_str());
  Log.verbose(F("Free heap: %d bytes" CR), ESP.getFreeHeap());
  Log.verbose(F("Free sketch space: %d bytes" CR), ESP.getFreeSketchSpace());
  Log.verbose(F("Chip ID: %x" CR), ESP.getEfuseMac());

  Log.verbose(F("Flash chip size: %d bytes" CR), ESP.getFlashChipSize());
  //  Log.verbose(F("Flash chip speed: %d Hz" CR), ESP.getFlashChipSpeed());
  Log.verbose(F("CPU frequency: %d Hz" CR), ESP.getCpuFreqMHz());
  Log.verbose(F("SDK version: %s" CR), ESP.getSdkVersion());

  logSection("Wifi Module Setup");
#ifdef M5_STATUS_LED
  WiFi.onEvent(onStationModeGotIP, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.onEvent(onStationModeDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
#endif
  wifiModuleSetup();
  logSection("MQTT Module Setup");
  mqttModuleSetup();
#ifdef LOCAL_CLIENT
  logSection("RS485 Module Setup");
  rs485Setup();
#endif
#ifdef REMOTE_CLIENT
  logSection("Find Remote Spa Setup");
  findSpaSetup();
  logSection("Spa Remote Communications Setup");
  spaCommunicationSetup();
#endif
  logSection("Web Server Setup");
  spaWebServerSetup();
  logSection("Spa Message Setup");
  spaMessageSetup();

#if defined(LOCAL_CONNECT) || defined(BRIDGE)
  logSection("Bridge Setup");
  bridgeSetup();
#endif
#ifdef M5_STATUS_LED
  ledControl.begin();
#endif
  logSection("Setup Complete");
}

void loop()
{
  esp_task_wdt_reset();
#ifdef LOCAL_CLIENT
  rs485Loop();
  rs485BootSafetyTick();
#endif
#ifdef spaEpaper
  spaEpaperLoop();
#endif
  wifiModuleLoop();

#ifdef LOCAL_CLIENT
  if (WiFi.status() == WL_CONNECTED && wifiOtaIsStarted() &&
      (rs485RetryPending() || (!rs485UartBegun() && !rs485SafeModeActive())))
  {
    (void)rs485EnsureUartBegun();
  }
#ifdef M5_STATUS_LED
  {
    Rs485LedAlert alert = Rs485LedAlert::None;
    if (WiFi.status() == WL_CONNECTED)
    {
      if (rs485SafeModeActive())
      {
        alert = Rs485LedAlert::SafeMode;
      }
      else if (rs485UartBegun() && rs485UartUptimeMs() >= 15000u &&
               rs485ValidFramesSinceBoot == 0)
      {
        alert = Rs485LedAlert::NoSpaData;
      }
    }
    ledControl.setRs485LedAlert(alert);
  }
#endif
#endif

  if (WiFi.status() == WL_CONNECTED)
  {
    mqttModuleLoop();
#ifdef REMOTE_CLIENT
    if (findSpaLoop())
    {
      if (!spaCommunicationLoop(getSpaIP()))
      {
        Log.verbose(F("[Main]: spaCommunicationLoop failed, client disconnected" CR));
        resetSpaCount();
      }
    }
#endif
    spaMessageLoop();
    spaWebServerLoop();
#if defined(LOCAL_CONNECT) || defined(BRIDGE)
    bridgeLoop();
#endif
  }
#ifdef M5_STATUS_LED
  ledControl.update();
#endif
}
