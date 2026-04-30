#ifndef MAIN_H
#define MAIN_H
#include <Arduino.h>

#ifndef SERIAL_BAUD
#define SERIAL_BAUD 115200
#endif

#ifndef LOG_LEVEL
#define LOG_LEVEL LOG_LEVEL_VERBOSE
#endif

#define VERSION "2.4.0"
// Compile-time string — do not use String(...).c_str() (dangling pointer if used as const char*).
#define BUILD __DATE__ " - " __TIME__

// GitHub links for web /state + GET /api/version (optional overrides in src/config.h).
#ifndef FIRMWARE_REPO_OWNER
#define FIRMWARE_REPO_OWNER "shomanjk"
#endif
#ifndef FIRMWARE_REPO_NAME
#define FIRMWARE_REPO_NAME "esp32_balboa_spa"
#endif
#ifndef FIRMWARE_REPO_README_URL
#define FIRMWARE_REPO_README_URL "https://github.com/" FIRMWARE_REPO_OWNER "/" FIRMWARE_REPO_NAME "/blob/main/README.md"
#endif
#ifndef FIRMWARE_REPO_RELEASES_URL
#define FIRMWARE_REPO_RELEASES_URL "https://github.com/" FIRMWARE_REPO_OWNER "/" FIRMWARE_REPO_NAME "/releases"
#endif
#ifndef FIRMWARE_REPO_RELEASES_LATEST_API_URL
#define FIRMWARE_REPO_RELEASES_LATEST_API_URL "https://api.github.com/repos/" FIRMWARE_REPO_OWNER "/" FIRMWARE_REPO_NAME "/releases/latest"
#endif

#define INITIAL_WDT_TIMEOUT 300 // Reset ESP32 if wifi is not connected within 5 minutes
#if defined(ESP32S3)
#warning "Need to look into S3 watchdog timer"
#define RUNNING_WDT_TIMEOUT 1200  // Reset ESP32 if no SPA messages are received for 60 seconds
#else
#define RUNNING_WDT_TIMEOUT 60  // Reset ESP32 if no SPA messages are received for 60 seconds
#endif

#define logSection(section)                                                  \
  Log.setShowLevel(false);                                                   \
  Log.notice(F("************* " section " **************" CR)); \
  Log.setShowLevel(true);

// Global Message Queues

#define BALBOA_MESSAGE_SIZE 50

extern QueueHandle_t spaWriteQueue;
extern QueueHandle_t spaReadQueue;

struct SpaReadQueueMessage
{
  u_int8_t message[BALBOA_MESSAGE_SIZE];
  int length;
};

struct SpaWriteQueueMessage
{
  u_int8_t message[BALBOA_MESSAGE_SIZE];
  int length;
};

extern String buildDefinitionString;

#ifdef LOCAL_CLIENT
#ifdef REMOTE_CLIENT
#error "Cannot define both LOCAL_CLIENT and REMOTE_CLIENT"
#endif
#endif

#ifndef LOCAL_CLIENT
#ifndef REMOTE_CLIENT
#error "Define either LOCAL_CLIENT or REMOTE_CLIENT"
#endif
#endif

#define GRAPH_MAX_READINGS 24 // Limited to 3-days here, but could go to 5-days = 40 as the data is issued  

#endif
