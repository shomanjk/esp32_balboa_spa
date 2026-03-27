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

#define WIFI_CONNECT_TIMEOUT 10000

const long gmtOffset_sec = GMT_OFFSET;
const int daylightOffset_sec = DAYLIGHT_OFFSET;
extern char gatewayName[20];

void wifiModuleSetup();
void wifiModuleLoop();
void notifyOfUpdateStarted();
void notifyOfUpdateEnded();
void wifiConnect();
void otaSetup();

String getStringTime();

#endif