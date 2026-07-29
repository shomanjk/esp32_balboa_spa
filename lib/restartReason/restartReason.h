#ifndef RESTART_REASON_H
#define RESTART_REASON_H
#include <Arduino.h>

/** Composite string for portal/MQTT: ESP reset text, plus soft intent only on ESP_RST_SW. */
String getLastRestartReason();
/** ESP-IDF reset reason text only (no soft RTC label). */
String getEspResetReasonText();
/** Soft RTC intent label (OTA Update, Web restart, …), or empty. */
String getLastRestartIntent();
void setLastRestartReason(String description);
String getLastRestartReasonDescription();

#define RR_MAGIC_NUMBER 0x5A5A5A5A
#define RR_MAXIMUM_DESCRIPTION_LENGTH 30

#endif
