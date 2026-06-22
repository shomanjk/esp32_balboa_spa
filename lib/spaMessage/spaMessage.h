#ifndef SPA_MESSAGE_H
#define SPA_MESSAGE_H
#include <Arduino.h>
#include <CircularBuffer.hpp>
#include "balboa.h"
#include "spaCommandDispatcher.h"

struct TempHistoryData;

extern RTC_NOINIT_ATTR SpaStatusData spaStatusData;
extern RTC_NOINIT_ATTR TempHistoryData tempHistoryData;
extern RTC_NOINIT_ATTR SpaConfigurationData spaConfigurationData;
extern RTC_NOINIT_ATTR SpaInformationData spaInformationData;
extern RTC_NOINIT_ATTR SpaSettings0x04Data spaSettings0x04Data;
extern RTC_NOINIT_ATTR SpaFilterSettingsData spaFilterSettingsData;
extern RTC_NOINIT_ATTR SpaPreferencesData spaPreferencesData;
extern RTC_NOINIT_ATTR WiFiModuleConfigurationData wiFiModuleConfigurationData;
extern RTC_NOINIT_ATTR SpaFaultLogData spaFaultLogData;

void spaMessageSetup();
void spaMessageLoop();

void sendMessageToSpa(uint8_t *data, int length);
void sendMessageToSpa(CircularBuffer<uint8_t, BALBOA_MESSAGE_SIZE> &data);
/** Queue a filter-settings read (`0x22` subcode `0x01`) to refresh `spaFilterSettingsData`. */
void spaRequestFilterSettings();
/** After a filter write, queue extra filter reads at ~2s intervals (controller apply lag). */
void spaScheduleFilterSettingsReadbackFollowup(uint8_t extraReads = 2);

String getMapDescription(uint8_t element, const std::map<uint8_t, const char*>& suppliedMap);
String spaFaultMessageForCode(uint8_t code, uint8_t totEntry);
String spaFormatFaultLogTime(const SpaFaultLogData &data);

#endif