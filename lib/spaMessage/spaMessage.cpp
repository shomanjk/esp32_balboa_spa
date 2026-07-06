#include "spaMessage.h"
#include <ArduinoLog.h>
#include <TickTwo.h>

#include <spaUtilities.h>
#include <Analytics.h>
#include <tempHistory.h>
#include <esp_task_wdt.h>

#include "../../src/main.h"
#include "balboa.h"
#include "spaMqttMessage.h"
#include <mqttModule.h>
#if MQTT_HA_DISCOVERY
#include <haMqttDiscovery.h>
#endif
#include "rs485.h"
#include "bridge.h"
#include <faultCapture.h>
#include <diagBridgeLog.h>

struct SpaFaultLogHistoryScan
{
  bool active;
  bool complete;
  bool error;
  uint8_t targetCount;
  uint8_t receivedCount;
  uint8_t pendingEntry;
  uint8_t pendingRetries;
  unsigned long lastSendMs;
  SpaFaultLogHistoryEntry entries[24];
};

#define TwoBit(value, bit) (((value) >> (bit)) & 0x03)

#define MAGIC_NUMBER 0x12345678

#define SPA_WRITE_QUEUE 10
#define SPA_READ_QUEUE 10

QueueHandle_t spaWriteQueue;
QueueHandle_t spaReadQueue;

// Global Variables
RTC_NOINIT_ATTR SpaStatusData spaStatusData;
RTC_NOINIT_ATTR SpaConfigurationData spaConfigurationData;
RTC_NOINIT_ATTR SpaInformationData spaInformationData;
RTC_NOINIT_ATTR SpaSettings0x04Data spaSettings0x04Data;
RTC_NOINIT_ATTR SpaFilterSettingsData spaFilterSettingsData;
RTC_NOINIT_ATTR SpaPreferencesData spaPreferencesData;
RTC_NOINIT_ATTR WiFiModuleConfigurationData wiFiModuleConfigurationData;
RTC_NOINIT_ATTR SpaFaultLogData spaFaultLogData;

// Analytics Data
RTC_NOINIT_ATTR AnalyticsData heatOnData;
RTC_NOINIT_ATTR AnalyticsData filterOnData;
RTC_NOINIT_ATTR TempHistoryData tempHistoryData;

// private functions
bool parseStatusMessage(u_int8_t *, int);
void parseInformationResponse(u_int8_t *, int);
void parseConfigurationResponse(u_int8_t *, int);
void parseWiFiModuleConfigurationResponse(u_int8_t *, int);
void parsePreferencesResponse(u_int8_t *, int);
void parseFaultResponse(u_int8_t *, int);
void parseFilterResponse(u_int8_t *, int);
void parseSettings0x04Response(u_int8_t *, int);
void configurationRequest();
void updateTemperatureHistory();

TickTwo temperatureHistory(updateTemperatureHistory, .75 * 60 * 1000); // First sample ~45s, then every 10 min

void spaMessageSetup()
{
  Log.verbose(F("[Mess]: spaMessageSetup()" CR));
  spaWriteQueue = xQueueCreate(SPA_WRITE_QUEUE, sizeof(struct SpaWriteQueueMessage *));
  spaReadQueue = xQueueCreate(SPA_READ_QUEUE, sizeof(struct SpaReadQueueMessage *));
  // put your setup code here, to run once:
  if (spaStatusData.magicNumber != MAGIC_NUMBER)
  {
    Log.verbose(F("[Mess]: spaStatusData.magicNumber: %x" CR), spaStatusData.magicNumber);
    spaStatusData = {};
    spaStatusData.magicNumber = MAGIC_NUMBER;
  }

  if (spaConfigurationData.magicNumber != MAGIC_NUMBER)
  {
    Log.verbose(F("[Mess]: spaConfigurationData.magicNumber: %x" CR), spaConfigurationData.magicNumber);
    spaConfigurationData = {};
    spaConfigurationData.magicNumber = MAGIC_NUMBER;
  }

  if (spaInformationData.magicNumber != MAGIC_NUMBER)
  {
    Log.verbose(F("[Mess]: spaInformationData.magicNumber: %x" CR), spaInformationData.magicNumber);

    spaInformationData = {};
    spaInformationData.magicNumber = MAGIC_NUMBER;
  }

  if (spaSettings0x04Data.magicNumber != MAGIC_NUMBER)
  {
    Log.verbose(F("[Mess]: spaSettings0x04Data.magicNumber: %x" CR), spaSettings0x04Data.magicNumber);

    spaSettings0x04Data = {};
    spaSettings0x04Data.magicNumber = MAGIC_NUMBER;
  }

  if (spaFilterSettingsData.magicNumber != MAGIC_NUMBER)
  {
    Log.verbose(F("[Mess]: spaFilterSettingsData.magicNumber: %x" CR), spaFilterSettingsData.magicNumber);

    spaFilterSettingsData = {};
    spaFilterSettingsData.magicNumber = MAGIC_NUMBER;
  }

  if (spaPreferencesData.magicNumber != MAGIC_NUMBER)
  {
    Log.verbose(F("[Mess]: spaPreferencesData.magicNumber: %x" CR), spaPreferencesData.magicNumber);

    spaPreferencesData = {};
    spaPreferencesData.magicNumber = MAGIC_NUMBER;
  }
  if (wiFiModuleConfigurationData.magicNumber != MAGIC_NUMBER)
  {
    Log.verbose(F("[Mess]: wiFiModuleConfigurationData.magicNumber: %x" CR), wiFiModuleConfigurationData.magicNumber);
    wiFiModuleConfigurationData = {};
    wiFiModuleConfigurationData.magicNumber = MAGIC_NUMBER;
  }
  if (spaFaultLogData.magicNumber != MAGIC_NUMBER)
  {
    Log.verbose(F("[Mess]: spaFaultLogData.magicNumber: %x" CR), spaFaultLogData.magicNumber);
    spaFaultLogData = {};
    spaFaultLogData.magicNumber = MAGIC_NUMBER;
  }
  else if (spaFaultLogData.faultCode != 0 && spaFaultLogData.faultMessage.length() == 0)
  {
    spaFaultLogData.faultMessage = spaFaultMessageForCode(spaFaultLogData.faultCode, spaFaultLogData.totEntry);
  }

  if (staleData(spaFilterSettingsData))
  {
    Log.verbose(F("[Mess]: Stale Filter Settings" CR));
  }
  if (staleData(spaFaultLogData))
  {
    Log.verbose(F("[Mess]: Stale Fault Log" CR));
  }
  if (staleData(spaInformationData))
  {
    Log.verbose(F("[Mess]: Stale Information" CR));
  }
  if (staleData(spaSettings0x04Data))
  {
    Log.verbose(F("[Mess]: Stale Settings 0x04" CR));
  }
  if (staleData(spaConfigurationData))
  {
    Log.verbose(F("[Mess]: Stale Configuration" CR));
  }
  if (staleData(spaPreferencesData))
  {
    Log.verbose(F("[Mess]: Stale Preferences" CR));
  }

  spaStatusData.heatOn = new Analytics(&heatOnData, "HeatOn");
  spaStatusData.filterOn = new Analytics(&filterOnData, "FilterOn");
  tempHistorySetup(&tempHistoryData);
  temperatureHistory.start();
}

static SpaFaultLogHistoryScan faultLogHistoryScan;

constexpr unsigned long kFaultLogHistoryEntryTimeoutMs = 30000;
constexpr uint8_t kFaultLogHistoryMaxEntryRetries = 3;

static void spaFaultLogHistoryQueueNextEntry(uint8_t entryIndex);

static void spaFaultLogHistoryAdvanceOrComplete(uint8_t nextEntry)
{
  if (nextEntry < faultLogHistoryScan.targetCount)
  {
    faultLogHistoryScan.pendingRetries = 0;
    spaFaultLogHistoryQueueNextEntry(nextEntry);
    return;
  }

  faultLogHistoryScan.complete = true;
  faultLogHistoryScan.active = false;
  spaRequestFaultLogEntry(0xFF);
}

static void spaFaultLogHistoryClearEntries()
{
  for (unsigned i = 0; i < 24; i++)
  {
    faultLogHistoryScan.entries[i] = {};
  }
}

static void spaFaultLogHistoryQueueNextEntry(uint8_t entryIndex)
{
  faultLogHistoryScan.pendingEntry = entryIndex;
  faultLogHistoryScan.lastSendMs = millis();
  spaRequestFaultLogEntry(entryIndex);
}

static void spaFaultLogHistoryHandleResponse(uint8_t totEntry, uint8_t currEntry, uint8_t code, uint8_t daysAgo, uint8_t hour,
                                             uint8_t minutes)
{
  if (currEntry != faultLogHistoryScan.pendingEntry)
  {
    Log.verbose(F("[Mess]: Fault log history ignored entry %u (waiting for %u)" CR), currEntry,
                faultLogHistoryScan.pendingEntry);
    return;
  }

  if (faultLogHistoryScan.targetCount == 0)
  {
    faultLogHistoryScan.targetCount = totEntry > 24 ? 24 : totEntry;
    if (faultLogHistoryScan.targetCount == 0)
    {
      faultLogHistoryScan.targetCount = 1;
    }
  }

  if (currEntry < 24)
  {
    SpaFaultLogHistoryEntry &slot = faultLogHistoryScan.entries[currEntry];
    slot.entry = currEntry;
    slot.code = code;
    slot.daysAgo = daysAgo;
    slot.hour = hour;
    slot.minutes = minutes;
    slot.valid = true;
  }

  faultLogHistoryScan.receivedCount++;
  faultLogHistoryScan.pendingRetries = 0;

  spaFaultLogHistoryAdvanceOrComplete(static_cast<uint8_t>(currEntry + 1));
}

static void spaFaultLogHistoryApplyLatest(u_int8_t *message, int length, uint8_t totEntry, uint8_t currEntry, uint8_t code,
                                          uint8_t daysAgo, uint8_t hour, uint8_t minutes)
{
  spaFaultLogData.crc = message[message[1]];
  spaFaultLogData.lastUpdate = getTime();
  for (int i = 0; i < length && i < BALBOA_MESSAGE_SIZE; i++)
  {
    spaFaultLogData.rawData[i] = message[i];
  }
  spaFaultLogData.rawDataLength = length;
  spaFaultLogData.totEntry = totEntry;
  spaFaultLogData.currEntry = currEntry;
  spaFaultLogData.faultCode = code;
  spaFaultLogData.daysAgo = daysAgo;
  spaFaultLogData.hour = hour;
  spaFaultLogData.minutes = minutes;
  spaFaultLogData.faultMessage = spaFaultMessageForCode(code, totEntry);
  publishSpaFaultLogData();
}

namespace
{
uint8_t filterSettingsFollowupReadsRemaining = 0;
unsigned long filterSettingsFollowupNextMs = 0;

void spaFilterSettingsReadbackFollowupTick()
{
  if (filterSettingsFollowupReadsRemaining == 0 || millis() < filterSettingsFollowupNextMs)
  {
    return;
  }
  spaRequestFilterSettings();
  filterSettingsFollowupReadsRemaining--;
  if (filterSettingsFollowupReadsRemaining > 0)
  {
    filterSettingsFollowupNextMs = millis() + 2000;
  }
}
} // namespace

void spaScheduleFilterSettingsReadbackFollowup(uint8_t extraReads)
{
  if (extraReads == 0)
  {
    return;
  }
  filterSettingsFollowupReadsRemaining = extraReads;
  filterSettingsFollowupNextMs = millis() + 2000;
}

void spaMessageLoop()
{
  // Log.verbose(F("[Mess]: spaMessageLoop - %d" CR), uxQueueMessagesWaiting(spaReadQueue));
  if (uxQueueMessagesWaiting(spaReadQueue) > 0)
  {
    SpaReadQueueMessage *message;
    if (xQueueReceive(spaReadQueue, &message, 0) == pdTRUE)
    {
      esp_task_wdt_reset();
      // Log.verbose(F("[Mess]: Queue Message Received: [%d]%s" CR), message->length, msgToString(message->message, message->length).c_str());
#if defined(LOCAL_CONNECT) || defined(BRIDGE)
      if (message->message[2] == id || message->message[2] == 0xff)
      {
        bridgeSend(message->message, message->length);
      }
#endif
      switch (message->message[4])
      {
      case Status_Message_Type:
        parseStatusMessage(message->message, message->length);
        break;
      case Filter_Cycles_Type:
        Log.verbose(F("[Mess]: Filter Cycles Response: %s" CR), msgToString(message->message, message->length).c_str());
        parseFilterResponse(message->message, message->length);
        break;
      case Information_Response_Type:
        Log.verbose(F("[Mess]: Information Response: %s" CR), msgToString(message->message, message->length).c_str());
        parseInformationResponse(message->message, message->length);
        break;
      case Settings_0x04_Response_Type:
        Log.verbose(F("[Mess]: Settings 0x04 Response: %s" CR), msgToString(message->message, message->length).c_str());
        parseSettings0x04Response(message->message, message->length);
        break;
      case Preferences_Type:
        Log.verbose(F("[Mess]: Preferences Response: %s" CR), msgToString(message->message, message->length).c_str());
        parsePreferencesResponse(message->message, message->length);
        break;
      case Fault_Log_Type:
        Log.verbose(F("[Mess]: Fault_Log_Type Response: %s" CR), msgToString(message->message, message->length).c_str());
        parseFaultResponse(message->message, message->length);
        break;
      case Configuration_Type:
        Log.verbose(F("[Mess]: Configuration Response: %s" CR), msgToString(message->message, message->length).c_str());
        parseConfigurationResponse(message->message, message->length);
        break;
      case WiFi_Module_Configuration_Type:
        Log.verbose(F("[Mess]: WiFi Module Configuration Response: %s" CR), msgToString(message->message, message->length).c_str());
        parseWiFiModuleConfigurationResponse(message->message, message->length);
        break;
      default:
        Log.verbose(F("[Mess]: Unknown Message Type: %x - %s" CR), message->message[4], msgToString(message->message, message->length).c_str());
      }
    }
    delete message;
  }
  else
  {
    if (!spaFaultLogHistoryIsActive() &&
        (staleData(spaConfigurationData) || staleData(spaSettings0x04Data) || staleData(spaFilterSettingsData) ||
         staleData(spaInformationData) || staleData(spaPreferencesData)))
    {
      configurationRequest();
    }
  }
  temperatureHistory.update();
  tempHistoryMaybePersist(&tempHistoryData);
  if (!spaFaultLogHistoryIsActive())
  {
    spaFilterSettingsReadbackFollowupTick();
  }
  spaFaultLogHistoryTimeoutTick();
}

void configurationRequest()
{
  if (spaFaultLogHistoryIsActive())
  {
    return;
  }

  unsigned char byte_array[100] = {0};
  int offset = 0;

  unsigned char config_request[] = CONFIGURATION_REQUEST;
  unsigned char settings_request[] = SETTINGS_0X04_REQUEST;
  unsigned char filter_settings_request[] = FILTER_SETTINGS_REQUEST;
  unsigned char information_request[] = INFORMATION_REQUEST;
  unsigned char fault_log_request[] = FAULT_LOG_REQUEST;
  unsigned char preferences_request[] = PREFERENCES_REQUEST;

  String request = "";

  if (staleData(spaConfigurationData) && retryRequest(spaConfigurationData))
  {
    append_request(byte_array, &offset, config_request, sizeof(config_request));
    spaConfigurationData.lastRequest = getTime();
    request += "Configuration ";
  }
  if (staleData(spaSettings0x04Data) && retryRequest(spaSettings0x04Data))
  {
    append_request(byte_array, &offset, settings_request, sizeof(settings_request));
    spaSettings0x04Data.lastRequest = getTime();
    request += "Settings ";
  }
  if (staleData(spaFilterSettingsData) && retryRequest(spaFilterSettingsData))
  {
    append_request(byte_array, &offset, filter_settings_request, sizeof(filter_settings_request));
    spaFilterSettingsData.lastRequest = getTime();
    request += "Filter ";
  }
  if (staleData(spaInformationData) && retryRequest(spaInformationData))
  {
    append_request(byte_array, &offset, information_request, sizeof(information_request));
    spaInformationData.lastRequest = getTime();
    request += "Information ";
  }
  if (staleData(spaFaultLogData) && retryRequest(spaFaultLogData) && !spaFaultLogHistoryIsActive())
  {
    append_request(byte_array, &offset, fault_log_request, sizeof(fault_log_request));
    spaFaultLogData.lastRequest = getTime();
    request += "FaultLog ";
  }
  if (staleData(spaPreferencesData) && retryRequest(spaPreferencesData))
  {
    append_request(byte_array, &offset, preferences_request, sizeof(preferences_request));
    spaPreferencesData.lastRequest = getTime();
    request += "Preferences ";
  }

  if (offset)
  {
    SpaWriteQueueMessage *messageToSend = new SpaWriteQueueMessage;
    messageToSend->length = offset;
    memcpy(messageToSend->message, byte_array, offset);
    if (xQueueSend(spaWriteQueue, &messageToSend, 0) != pdTRUE)
    {
      Log.error(F("[Mess]: SPA Write Queue full, dropped %s" CR), msgToString(messageToSend->message, messageToSend->length).c_str());
      delete messageToSend;
    }
    else
    {
      Log.verbose(F("[Mess]: Queuing request to spa '%s' - %s" CR), request.c_str(), msgToString(messageToSend->message, messageToSend->length).c_str());
    }
  }
}

/*

Preferences Response

A Preferences Response is sent by the Main Board after a client sends the appropriate Settings Request (using the same Channel as the request) or when a client sends a Set Preference Request (using the broadcast Channel).

Type Code: 0x26

Length: 23

Arguments:

Byte(s)	Name	Description/Values
0	??	0
1	Reminders	0=OFF, 1=OFF
2	??	0
3	Temperature Scale	0=1°F, 1=0.5°C
4	Clock Mode	0=12-hour, 1=24-hour
5	Cleanup Cycle	0=OFF, 1-8 (30 minute increments)
6	Dolphin Address	0=none, 1-7=address
7	??	0
8	M8 Artificial Intelligence	0=OFF, 1=ON
9-17	??	0


*/

void parsePreferencesResponse(u_int8_t *message, int length)
{
  spaPreferencesData.crc = message[message[1]];
  spaPreferencesData.lastUpdate = getTime();
  spaPreferencesData.rawDataLength = length;
  for (int i = 0; i < length && i < BALBOA_MESSAGE_SIZE; i++)
  {
    spaPreferencesData.rawData[i] = message[i];
  }

  u_int8_t *hexArray = message + 5;

  spaPreferencesData.reminders = hexArray[1];
  spaPreferencesData.tempScale = hexArray[3];
  spaPreferencesData.clockMode = hexArray[4];
  spaPreferencesData.cleanupCycle = hexArray[5];
  spaPreferencesData.dolphinAddress = hexArray[6];
  spaPreferencesData.m8AI = hexArray[8];

  Log.verbose(F("[Mess]: Preferences Response: %s" CR), msgToString(hexArray, length - 7).c_str());
  void publishSpaPreferencesData();
}

/*

WiFi Module Configuration Response

A WiFi Module Configuration Response is sent by the WiFi Module when the App sends an Existing Client Request.

Type Code: 0x94

Length: 29

Arguments:

Byte(s)	Name	Values
0-2	??	??
3-8	Full MAC address	Varies
9-16	??	0
17-19	MAC address: OUI	00:15:27 (Balboa Instruments)
20-21	??	0xFF
22-24	MAC address: NIC-specific	Varies

*/

void parseWiFiModuleConfigurationResponse(u_int8_t *message, int length)
{
  wiFiModuleConfigurationData.crc = message[message[1]];
  wiFiModuleConfigurationData.lastUpdate = getTime();

  u_int8_t *hexArray = message + 5;

  snprintf(wiFiModuleConfigurationData.macAddress, sizeof(wiFiModuleConfigurationData.macAddress), "%02x:%02x:%02x:%02x:%02x:%02x", hexArray[3], hexArray[4], hexArray[5], hexArray[6], hexArray[7], hexArray[8]);

  // Log.verbose(F("[Mess]: WiFi Module Configuration Response: %s" CR), msgToString(hexArray, length - 7).c_str());
  publishWiFiModuleConfigurationData();
}

/*

Configuration Response

A Configuration Response is sent by the Main Board after a client sends the appropriate Settings Request.

Type Code: 0x2E

Length: 11

Arguments:

Byte	Name	Values
0	Pumps 1-4	Bits N to N+1: Pump N/2+1 (0=None, 1=1-speed, 2=2-speed)
1	Pumps 5-6	Bits 0-1: Pump 5, Bits 6-7: Pump 6 (0=None, 1=1-speed, 2=2-speed)
2	Lights	Bits 0-1: Light 1, Bits 6-7: Light 2 (0=None, 1=Present)
3	Flags Byte 3	?Bits 0-1: Blower, Bit 7: Circulation Pump?
4	Flags Byte 4	?Bit 0: Aux 1, Bit 1: Aux 2, Bits 4-5: Mister?
5	??	0x00=??, 0x68=??



*/

void parseConfigurationResponse(u_int8_t *message, int length)
{
  spaConfigurationData.crc = message[message[1]];
  spaConfigurationData.lastUpdate = getTime();

  for (int i = 0; i < length && i < BALBOA_MESSAGE_SIZE; i++)
  {
    spaConfigurationData.rawData[i] = message[i];
  }
  spaConfigurationData.rawDataLength = length;

  u_int8_t *hexArray = message + 5;

  spaConfigurationData.pump1 = TwoBit(hexArray[0], 0);
  spaConfigurationData.pump2 = TwoBit(hexArray[0], 2);
  spaConfigurationData.pump3 = TwoBit(hexArray[0], 4);
  spaConfigurationData.pump4 = TwoBit(hexArray[0], 6);

  spaConfigurationData.pump5 = TwoBit(hexArray[1], 0);
  spaConfigurationData.pump6 = TwoBit(hexArray[1], 2);

  spaConfigurationData.light1 = TwoBit(hexArray[2], 0);
  spaConfigurationData.light2 = TwoBit(hexArray[2], 2);

  spaConfigurationData.blower = TwoBit(hexArray[3], 0);
  spaConfigurationData.circulationPump = TwoBit(hexArray[3], 6);

  spaConfigurationData.aux1 = bitRead(hexArray[4], 0);
  spaConfigurationData.aux2 = bitRead(hexArray[4], 1);
  spaConfigurationData.mister = TwoBit(hexArray[4], 4);

  // Log.verbose(F("[Mess]: Configuration Response: %s" CR), msgToString(hexArray, length - 7).c_str());
  publishSpaConfigurationData();
#if MQTT_HA_DISCOVERY
  publishHomeAssistantDiscoveryExpanded();
#endif
}

/*

Information Response

The Main Board sends a Filter Cycles Message when a client sends the appropriate Settings Request.

Type Code: 0x24

Length: 25

Arguments:

Byte(s)	Name	Description/Values
0-3	Software ID (SSID)	Displayed (in decimal) as "M<byte 0>_<byte 1> V<byte 2>[.<byte 3>]"
4-11	System Model Number	ASCII-encoded string
12	Current Configuration Setup Number	Refer to controller Tech Sheets
13-16	Configuration Signature	Checksum of the system configuration file
17	?Heater Voltage?	?0x01=240?
18	?Heater Type?	?0x06,0x0A=Standard?
19-20	DIP Switch Settings	LSB-first (bit 0 of Byte 19 is position 1)
*/

// 7e 1a 0a bf 24 64 c9 2c 00 53 52 42 50 35 30 31 58 03 09 57 fa 83 01 06 02 00 1f 7e 7e 20 ff af 13 00 00 ff 0c 1f 00 00 48 00 81 00 00 00 00 00 00 00

void parseInformationResponse(u_int8_t *message, int length)
{

  spaInformationData.crc = message[message[1]];
  spaInformationData.lastUpdate = getTime();
  for (int i = 0; i < length && i < BALBOA_MESSAGE_SIZE; i++)
  {
    spaInformationData.rawData[i] = message[i];
  }
  spaInformationData.rawDataLength = length;

  u_int8_t *hexArray = message + 5;
  snprintf(spaInformationData.softwareID, sizeof(spaInformationData.softwareID), "M%d_%d V%d.%d", hexArray[0], hexArray[1], hexArray[2], hexArray[3]);
  snprintf(spaInformationData.model, sizeof(spaInformationData.model), "%c%c%c%c%c%c%c%c", hexArray[4], hexArray[5], hexArray[6], hexArray[7], hexArray[8], hexArray[9], hexArray[10], hexArray[11]);
  spaInformationData.setupNumber = hexArray[12];
  spaInformationData.voltage = hexArray[17];
  spaInformationData.heaterType = hexArray[18];
  snprintf(spaInformationData.dipSwitch, sizeof(spaInformationData.dipSwitch), "%x%x", hexArray[20], hexArray[19]);

  // Log.verbose(F("[Mess]: Information Response: %s" CR), msgToString(hexArray, length - 7).c_str());
  publishSpaInformationData();
#if MQTT_HA_DISCOVERY
  publishHomeAssistantDiscoveryExpanded();
#endif
}

/*
Byte	Name	Description/Values
0	?Spa State?	0x00=Running, 0x01=Initializing, 0x05=Hold Mode, ?0x14=A/B Temps ON?, 0x17=Test Mode
1	?Initialization Mode?	0x00=Idle, 0x01=Priming Mode, 0x02=?Fault?, 0x03=Reminder, 0x04=?Stage 1?, 0x05=?Stage 3?, 0x42=?Stage 2?
2	Current Temperature	Temperature (scaled by Temperature Scale), 0xFF if unknown
3	Time: Hour	0-23
4	Time: Minute	0-59
5	Heating Mode	0=Ready, 1=Rest, 3=Ready-in-Rest
6	Reminder Type	0x00=None, 0x03/0x04=Clean filter, 0x08=Change water, 0x09=Check sanitizer, 0x0A=Check pH, 0x1E=Fault (others model-specific; byte unreliable while spaState=Initializing)
7	Sensor A Temperature / Hold Timer	Minutes if Hold Mode else Temperature (scaled by Temperature Scale) if A/B Temps else 0x01 if Test Mode else 0x00
8	Sensor B Temperature	Temperature (scaled by Temperature Scale) if A/B Temps else 0x00
9	Flags Byte 9	Temperature Scale, Clock Mode, Filter Mode (see below)
10	Flags Byte 10	Heating, Temperature Range (see below)
11	Flags Byte 11	Pumps 1-4 Status (see below)
12	Flags Byte 12	Pumps 5-6 Status (see below)
13	Flags Byte 13	Circulation Pump Status, Blower Status (see below)
14	Flags Byte 14	Lights 1-2 Status (see below)
15	Mister	0=OFF, 1=ON
16	??	0
17	??	0
18	Flags Byte 18	Notification Type (see below)
19	Flags Byte 19	Circulation Cycle, Notification (see below)
20	Set Temperature	Temperature (scaled by Temperature Scale)
21	Flags Byte 21	(see below)
22-23	??	0
24	M8 Cycle Time	0=OFF; 30, 60, 90, or 120 (in minutes)
25-26	??	0
*/

bool parseStatusMessage(u_int8_t *message, int length)
{

  if (spaStatusData.crc != message[message[1]])
  {
    spaStatusData.rawData[0] = message[0];
    spaStatusData.crc = message[message[1]];
    spaStatusData.lastUpdate = getTime();
    for (int i = 0; i < length && i < BALBOA_MESSAGE_SIZE; i++)
    {
      spaStatusData.rawData[i] = message[i];
    }
    spaStatusData.rawDataLength = length;

    u_int8_t *hexArray = message + 5;
    spaStatusData.spaState = hexArray[0];
    spaStatusData.initMode = hexArray[1];
    spaStatusData.currentTemp = (hexArray[2] != 0xff ? (hexArray[9] & 0x01 ? (float)hexArray[2] / 2 : hexArray[2]) : spaStatusData.currentTemp);
    // Combine hour and minute into a time string
    uint8_t hour = hexArray[3];
    uint8_t minute = hexArray[4];
    sprintf(spaStatusData.time, "%02d:%02d", hour, minute);

    spaStatusData.heatingMode = hexArray[5];
    spaStatusData.reminderType = hexArray[6];
    spaStatusData.sensorA = hexArray[7];
    spaStatusData.sensorB = hexArray[8];

    spaStatusData.tempScale = hexArray[9] & 0x01;
    spaStatusData.clockMode = hexArray[9] & 0x02;
    spaStatusData.filterMode = TwoBit(hexArray[9], 2);
    spaStatusData.filterOn->add(spaStatusData.filterMode);

    spaStatusData.panelLocked = hexArray[9] & 0x20;
    spaStatusData.tempRange = bitRead(hexArray[10], 2);
    spaStatusData.needsHeat = bitRead(hexArray[10], 3);
    spaStatusData.heatingState = TwoBit(hexArray[10], 4);
    spaStatusData.heatOn->add(spaStatusData.heatingState);

    spaStatusData.pump1 = TwoBit(hexArray[11], 0);
    spaStatusData.pump2 = TwoBit(hexArray[11], 2);
    spaStatusData.pump3 = TwoBit(hexArray[11], 4);
    spaStatusData.pump4 = TwoBit(hexArray[11], 6);
    spaStatusData.pump5 = TwoBit(hexArray[12], 0);
    spaStatusData.pump6 = TwoBit(hexArray[12], 2);

    spaStatusData.circ = bitRead(hexArray[13], 1);
    spaStatusData.blower = TwoBit(hexArray[13], 2);

    spaStatusData.light1 = bitRead(hexArray[14], 0);
    spaStatusData.light2 = bitRead(hexArray[14], 2);

    spaStatusData.mister = hexArray[15];

    spaStatusData.notification = hexArray[18];
    spaStatusData.flags19 = hexArray[19];

    spaStatusData.setTemp = (spaStatusData.tempScale ? (float)hexArray[20] / 2 : hexArray[20]);
    if (spaStatusData.tempRange)
    {
      spaStatusData.highSetTemp = (spaStatusData.tempScale ? (float)hexArray[20] / 2 : hexArray[20]);
    }
    else
    {
      spaStatusData.lowSetTemp = (spaStatusData.tempScale ? (float)hexArray[20] / 2 : hexArray[20]);
    }

    spaStatusData.settingsLock = bitRead(hexArray[21], 3);
    
    spaStatusData.m8CycleTime = hexArray[24];

    spaStatusData.filterOnTimeToday = spaStatusData.filterOn->today();
    spaStatusData.filterOnTimeYesterday = spaStatusData.filterOn->yesterday();
    spaStatusData.heaterOnTimeToday = spaStatusData.heatOn->today();
    spaStatusData.heaterOnTimeYesterday = spaStatusData.heatOn->yesterday();

    Log.verbose(F("[Mess]: Status Response: %s" CR), msgToString(hexArray, length - 7).c_str());

    publishSpaStatusData();
    return true;
  }
  return false;
}

void parseFaultResponse(u_int8_t *message, int length)
{
  u_int8_t *hexArray = message + 5;
  const uint8_t totEntry = hexArray[0];
  const uint8_t currEntry = hexArray[1];
  const uint8_t code = hexArray[2];
  const uint8_t daysAgo = hexArray[3];
  const uint8_t hour = hexArray[4];
  const uint8_t minutes = hexArray[5];

  if (faultLogHistoryScan.active)
  {
    spaFaultLogHistoryHandleResponse(totEntry, currEntry, code, daysAgo, hour, minutes);
    Log.verbose(F("[Mess]: Fault Log Response (history scan): %s" CR), msgToString(hexArray, length - 7).c_str());
    return;
  }

  spaFaultLogHistoryApplyLatest(message, length, totEntry, currEntry, code, daysAgo, hour, minutes);
  Log.verbose(F("[Mess]: Fault Log Response: %s" CR), msgToString(hexArray, length - 7).c_str());
}

void parseFilterResponse(u_int8_t *message, int length)
{
  spaFilterSettingsData.crc = message[message[1]];
  spaFilterSettingsData.lastUpdate = getTime();
  for (int i = 0; i < length && i < BALBOA_MESSAGE_SIZE; i++)
  {
    spaFilterSettingsData.rawData[i] = message[i];
  }
  spaFilterSettingsData.rawDataLength = length;

  u_int8_t *hexArray = message + 5;
  spaFilterSettingsData.filt1Hour = hexArray[0];
  spaFilterSettingsData.filt1Minute = hexArray[1];
  spaFilterSettingsData.filt1DurationHour = hexArray[2];
  spaFilterSettingsData.filt1DurationMinute = hexArray[3];
  spaFilterSettingsData.filt2Enable = bitRead(hexArray[4], 7); // Byte 4 (Bits 7)
  spaFilterSettingsData.filt2Hour = hexArray[4] & 0x7f;        // Byte 4 (Bits 0-6)
  spaFilterSettingsData.filt2Minute = hexArray[5];
  spaFilterSettingsData.filt2DurationHour = hexArray[6];
  spaFilterSettingsData.filt2DurationMinute = hexArray[7];
  spaNormalizeFilter2Cache(spaFilterSettingsData);

  // Log.verbose(F("[Mess]: Filter Response: %s" CR), msgToString(hexArray, length - 7).c_str());
  publishSpaFilterSettingsData();
}

void parseSettings0x04Response(u_int8_t *message, int length)
{
  spaSettings0x04Data.crc = message[message[1]];
  spaSettings0x04Data.lastUpdate = getTime();
  for (int i = 0; i < length && i < BALBOA_MESSAGE_SIZE; i++)
  {
    spaSettings0x04Data.rawData[i] = message[i];
  }
  spaSettings0x04Data.rawDataLength = length;

  u_int8_t *hexArray = message + 5;

  // Log.verbose(F("[Mess]: Settings 0x04 Response: %s" CR), msgToString(hexArray, length - 7).c_str());
  publishSpaSettings0x04Data();
}

void spaRequestFilterSettings()
{
  unsigned char filter_settings_request[] = FILTER_SETTINGS_REQUEST;
  sendMessageToSpa(filter_settings_request, sizeof(filter_settings_request));
  spaFilterSettingsData.lastRequest = getTime();
}

void spaRequestPreferences()
{
  unsigned char preferences_request[] = PREFERENCES_REQUEST;
  sendMessageToSpa(preferences_request, sizeof(preferences_request));
  spaPreferencesData.lastRequest = getTime();
}

void spaRequestFaultLogEntry(uint8_t entry)
{
  CircularBuffer<uint8_t, BALBOA_MESSAGE_SIZE> frame;
  frame.push(0x0a);
  frame.push(0xbf);
  frame.push(0x22);
  frame.push(0x20);
  frame.push(entry);
  frame.push(0x00);
  addCRC(frame);
  sendMessageToSpa(frame);
  spaFaultLogData.lastRequest = getTime();
}

void sendMessageToSpa(uint8_t *data, int length)
{
  SpaWriteQueueMessage *messageToSend = new SpaWriteQueueMessage;
  messageToSend->length = length;
  memcpy(messageToSend->message, data, length);
  if (xQueueSend(spaWriteQueue, &messageToSend, 0) != pdTRUE)
  {
    Log.error(F("[Mess]: SPA Write Queue full, dropped %s" CR), msgToString(messageToSend->message, messageToSend->length).c_str());
#if defined(DIAG_FAULT_CAPTURE)
    faultCaptureAppendf("[fault] spaWriteQueue full len=%d", messageToSend->length);
#endif
    delete messageToSend;
  }
  else
  {
    Log.verbose(F("[Mess]: Queuing message to spa %s" CR), msgToString(messageToSend->message, messageToSend->length).c_str());
    BRIDGE_LOG_NOISY(F("[BridgeDiag]: queued ms=%lu depth=%u frame=%s" CR),
               millis(),
               static_cast<unsigned int>(uxQueueMessagesWaiting(spaWriteQueue)),
               msgToString(messageToSend->message, messageToSend->length).c_str());
  }
}

void sendMessageToSpa(CircularBuffer<uint8_t, BALBOA_MESSAGE_SIZE> &data)
{
  SpaWriteQueueMessage *messageToSend = new SpaWriteQueueMessage;
  messageToSend->length = data.size();
  for (int i = 0; i < data.size() && i < BALBOA_MESSAGE_SIZE; i++)
  {
    messageToSend->message[i] = data[i];
  }
  if (xQueueSend(spaWriteQueue, &messageToSend, 0) != pdTRUE)
  {
    Log.error(F("[Mess]: SPA Write Queue full, dropped %s" CR), msgToString(messageToSend->message, messageToSend->length).c_str());
#if defined(DIAG_FAULT_CAPTURE)
    faultCaptureAppendf("[fault] spaWriteQueue full len=%d", messageToSend->length);
#endif
    delete messageToSend;
  }
  else
  {
    Log.verbose(F("[Mess]: Queuing message to spa %s" CR), msgToString(messageToSend->message, messageToSend->length).c_str());
    BRIDGE_LOG_NOISY(F("[BridgeDiag]: queued ms=%lu depth=%u frame=%s" CR),
               millis(),
               static_cast<unsigned int>(uxQueueMessagesWaiting(spaWriteQueue)),
               msgToString(messageToSend->message, messageToSend->length).c_str());
  }
}

String getMapDescription(uint8_t element, const std::map<uint8_t, const char *> &suppliedMap)
{
  auto it = suppliedMap.find(element);
  if (it != suppliedMap.end())
  {
    return String(it->second);
  }
  char buf[24];
  snprintf(buf, sizeof(buf), "Unknown (0x%02X)", element);
  return String(buf);
}

const char *spaBlowerBinaryLabel(uint8_t blower)
{
  return spaBlowerIsOn(blower) ? "On" : "Off";
}

String spaReminderText(uint8_t reminderType, uint8_t spaState)
{
  if (reminderType == 0x00)
  {
    return String("None");
  }
  // Byte 6 is not a reliable reminder id while the pack is still booting (often 0x13 = hour 19
  // or other transient values; 0x13 is also the status *message* type, not a reminder label).
  if (spaState == 0x01)
  {
    return String("None");
  }
  auto it = reminderTypeMap.find(reminderType);
  if (it != reminderTypeMap.end())
  {
    return String(it->second);
  }
  // Panel sent a reminder id we do not have in reminderTypeMap yet (model/firmware-specific).
  return String("Maintenance reminder");
}

String spaReminderHintText(uint8_t reminderType, uint8_t spaState)
{
  if (reminderType == 0x00)
  {
    return String("");
  }
  if (spaState == 0x01)
  {
    return String("");
  }
  auto it = reminderHintMap.find(reminderType);
  if (it != reminderHintMap.end())
  {
    return String(it->second);
  }
  return String("Stays until cleared on the spa panel. Repeat interval varies by reminder "
                "type and manufacturer.");
}

bool spaReminderIsActive(uint8_t reminderType, uint8_t spaState, uint8_t initMode)
{
  if (reminderType == 0x00)
  {
    return false;
  }
  // Byte 6 is unreliable while the pack is booting or in priming mode.
  if (spaState == 0x01 || initMode == 0x01)
  {
    return false;
  }
  return true;
}

bool spaReminderIsFault(uint8_t reminderType)
{
  return reminderType == 0x1E;
}

bool spaPreferencesRemindersEnabled(uint8_t reminders)
{
  return (reminders & 0x01u) != 0;
}

String spaPreferencesRemindersText(uint8_t reminders)
{
  return spaPreferencesRemindersEnabled(reminders) ? String("On") : String("Off");
}

String spaFaultMessageForCode(uint8_t code, uint8_t totEntry)
{
  if (code == 0 && totEntry == 0)
  {
    return String("None");
  }
  return getMapDescription(code, faultCodeMap);
}

String spaFormatFaultLogTime(const SpaFaultLogData &data)
{
  return spaFormatFaultLogEntryTime(data.daysAgo, data.hour, data.minutes, data.faultCode, data.totEntry);
}

String spaFormatFaultLogEntryTime(uint8_t daysAgo, uint8_t hour, uint8_t minutes, uint8_t code, uint8_t totEntry)
{
  if (code == 0 && totEntry == 0)
  {
    return String("None");
  }
  char buf[48];
  snprintf(buf, sizeof(buf), "%u days ago %02u:%02u", daysAgo, hour, minutes);
  return String(buf);
}

const char *spaFaultLogSeverityText(uint8_t code)
{
  switch (code)
  {
  case 19:
  case 37:
  case 18:
  case 21:
    return "info";
  case 15:
  case 16:
  case 26:
  case 28:
    return "warning";
  case 17:
  case 20:
  case 22:
  case 27:
  case 29:
  case 30:
  case 31:
  case 32:
  case 34:
  case 35:
  case 36:
    return "alert";
  default:
    return code == 0 ? "info" : "warning";
  }
}

bool spaFaultLogHistoryStart()
{
  if (faultLogHistoryScan.active)
  {
    if (millis() - faultLogHistoryScan.lastSendMs <= kFaultLogHistoryEntryTimeoutMs)
    {
      return false;
    }
    faultLogHistoryScan.error = true;
    faultLogHistoryScan.active = false;
    spaRequestFaultLogEntry(0xFF);
  }
  faultLogHistoryScan = {};
  faultLogHistoryScan.active = true;
  faultLogHistoryScan.targetCount = spaFaultLogData.totEntry > 24 ? 24 : spaFaultLogData.totEntry;
  if (faultLogHistoryScan.targetCount == 0)
  {
    faultLogHistoryScan.targetCount = 24;
  }
  spaFaultLogHistoryClearEntries();
  spaFaultLogHistoryQueueNextEntry(0);
  return true;
}

bool spaFaultLogHistoryIsActive()
{
  return faultLogHistoryScan.active;
}

bool spaFaultLogHistoryIsComplete()
{
  return faultLogHistoryScan.complete;
}

bool spaFaultLogHistoryHasError()
{
  return faultLogHistoryScan.error;
}

uint8_t spaFaultLogHistoryProgress()
{
  return faultLogHistoryScan.receivedCount;
}

uint8_t spaFaultLogHistoryTargetCount()
{
  return faultLogHistoryScan.targetCount;
}

uint8_t spaFaultLogHistoryPendingEntry()
{
  return faultLogHistoryScan.pendingEntry;
}

const SpaFaultLogHistoryEntry *spaFaultLogHistoryEntries()
{
  return faultLogHistoryScan.entries;
}

uint8_t spaFaultLogHistoryEntryCount()
{
  uint8_t count = 0;
  for (unsigned i = 0; i < 24; i++)
  {
    if (faultLogHistoryScan.entries[i].valid)
    {
      count++;
    }
  }
  return count;
}

void spaFaultLogHistoryTimeoutTick()
{
  if (!faultLogHistoryScan.active)
  {
    return;
  }
  if (millis() - faultLogHistoryScan.lastSendMs <= kFaultLogHistoryEntryTimeoutMs)
  {
    return;
  }

  if (faultLogHistoryScan.pendingRetries + 1 >= kFaultLogHistoryMaxEntryRetries)
  {
    Log.warning(F("[Mess]: Fault log history entry %u timed out after %u tries — skipping" CR),
                faultLogHistoryScan.pendingEntry, kFaultLogHistoryMaxEntryRetries);
    if (faultLogHistoryScan.receivedCount == 0)
    {
      faultLogHistoryScan.error = true;
      faultLogHistoryScan.active = false;
      spaRequestFaultLogEntry(0xFF);
      return;
    }
    spaFaultLogHistoryAdvanceOrComplete(static_cast<uint8_t>(faultLogHistoryScan.pendingEntry + 1));
    return;
  }

  faultLogHistoryScan.pendingRetries++;
  faultLogHistoryScan.lastSendMs = millis();
  Log.verbose(F("[Mess]: Fault log history retry entry %u (attempt %u)" CR), faultLogHistoryScan.pendingEntry,
              faultLogHistoryScan.pendingRetries + 1);
  spaRequestFaultLogEntry(faultLogHistoryScan.pendingEntry);
}

void updateTemperatureHistory()
{
  temperatureHistory.interval(TEMP_SAMPLE_INTERVAL_MS);
  tempHistorySample(&tempHistoryData, spaStatusData.currentTemp);
}