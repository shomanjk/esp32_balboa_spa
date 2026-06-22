#include "spaConfigExport.h"

#include <ArduinoLog.h>
#include <time.h>
#include <strings.h>

#include "spaMessage.h"
#include "../../src/main.h"
#include <spaUtilities.h>
#include <WiFi.h>

namespace
{
String rawFrameHexUpper(const uint8_t *data, uint8_t len, size_t maxShow = 96)
{
  String out;
  const size_t n = static_cast<size_t>(len) <= maxShow ? static_cast<size_t>(len) : maxShow;
  for (size_t i = 0; i < n; ++i)
  {
    if (i > 0)
    {
      out += ' ';
    }
    char b[4];
    snprintf(b, sizeof(b), "%02X", static_cast<unsigned>(data[i]));
    out += b;
  }
  if (static_cast<size_t>(len) > maxShow)
  {
    out += " ...";
  }
  return out;
}

String trimField(const char *s)
{
  if (s == nullptr)
  {
    return String();
  }
  String v = String(s);
  v.trim();
  return v;
}

String informationSignatureHex()
{
  constexpr unsigned kNeed = 5 + 17;
  if (spaInformationData.rawDataLength < kNeed)
  {
    return String();
  }
  const uint8_t *p = spaInformationData.rawData + 5;
  char buf[16];
  snprintf(buf, sizeof(buf), "%02X%02X%02X%02X", static_cast<unsigned>(p[13]), static_cast<unsigned>(p[14]),
           static_cast<unsigned>(p[15]), static_cast<unsigned>(p[16]));
  return String(buf);
}

void appendIso8601Utc(JsonObject root, const char *key)
{
  const time_t now = getTime();
  struct tm tmUtc;
  if (gmtime_r(&now, &tmUtc) == nullptr)
  {
    root[key] = static_cast<long>(now);
    return;
  }
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmUtc);
  root[key] = buf;
}

void appendApplied(JsonObject report, const char *section, const SpaCommandResult &result)
{
  JsonObject row = report["applied"].createNestedObject();
  row["section"] = section;
  row["result"] = result.accepted ? "accepted" : "rejected";
  if (!result.accepted && result.reason)
  {
    row["reason"] = result.reason;
  }
}

void appendSkipped(JsonObject report, const char *section, const char *reason)
{
  JsonObject row = report["skipped"].createNestedObject();
  row["section"] = section;
  row["reason"] = reason;
}

void appendFailed(JsonObject report, const char *section, const char *reason)
{
  JsonObject row = report["failed"].createNestedObject();
  row["section"] = section;
  row["reason"] = reason;
}

} // namespace

void spaConfigAppendFilterGetJson(JsonObject root)
{
  const bool ready = spaFilterSettingsData.lastUpdate != 0;
  root["ready"] = ready;
  root["lastUpdate"] = static_cast<long>(spaFilterSettingsData.lastUpdate);
  if (spaHasFreshStatus() && spaStatusData.time[0] != '\0')
  {
    root["panelClockHint"] = spaStatusData.time;
  }
  JsonObject f1 = root.createNestedObject("filter1");
  f1["startHour"] = spaFilterSettingsData.filt1Hour;
  f1["startMinute"] = spaFilterSettingsData.filt1Minute;
  f1["durationHour"] = spaFilterSettingsData.filt1DurationHour;
  f1["durationMinute"] = spaFilterSettingsData.filt1DurationMinute;
  JsonObject f2 = root.createNestedObject("filter2");
  f2["enabled"] = spaFilterSettingsData.filt2Enable;
  f2["startHour"] = spaFilterSettingsData.filt2Hour;
  f2["startMinute"] = spaFilterSettingsData.filt2Minute;
  f2["durationHour"] = spaFilterSettingsData.filt2DurationHour;
  f2["durationMinute"] = spaFilterSettingsData.filt2DurationMinute;
}

void spaConfigAppendExportJson(JsonObject root)
{
  root["schemaVersion"] = 1;
  appendIso8601Utc(root, "exportedAt");

  JsonObject gateway = root.createNestedObject("gateway");
  gateway["version"] = VERSION;
  gateway["build"] = BUILD;
  if (WiFi.getHostname())
  {
    gateway["hostname"] = WiFi.getHostname();
  }

  JsonObject readiness = root.createNestedObject("readiness");
  readiness["filter"] = spaFilterSettingsData.lastUpdate != 0;
  readiness["information"] = spaInformationData.lastUpdate != 0;
  readiness["configuration"] = spaConfigurationData.lastUpdate != 0;
  readiness["preferences"] = spaPreferencesData.lastUpdate != 0;
  readiness["settings0x04"] = spaSettings0x04Data.lastUpdate != 0;
  readiness["faultLog"] = spaFaultLogData.lastUpdate != 0;
  readiness["status"] = spaStatusData.lastUpdate != 0;

  JsonObject writable = root.createNestedObject("writable");
  if (spaFilterSettingsData.lastUpdate != 0)
  {
    JsonObject filter = writable.createNestedObject("filter");
    JsonObject f1 = filter.createNestedObject("filter1");
    f1["startHour"] = spaFilterSettingsData.filt1Hour;
    f1["startMinute"] = spaFilterSettingsData.filt1Minute;
    f1["durationHour"] = spaFilterSettingsData.filt1DurationHour;
    f1["durationMinute"] = spaFilterSettingsData.filt1DurationMinute;
    JsonObject f2 = filter.createNestedObject("filter2");
    f2["enabled"] = spaFilterSettingsData.filt2Enable;
    f2["startHour"] = spaFilterSettingsData.filt2Hour;
    f2["startMinute"] = spaFilterSettingsData.filt2Minute;
    f2["durationHour"] = spaFilterSettingsData.filt2DurationHour;
    f2["durationMinute"] = spaFilterSettingsData.filt2DurationMinute;
  }
  if (spaHasFreshStatus() && spaStatusData.time[0] != '\0')
  {
    const char *t = spaStatusData.time;
    if (strlen(t) >= 5 && t[2] == ':')
    {
      JsonObject clock = writable.createNestedObject("panelClock");
      clock["hour"] = (t[0] - '0') * 10 + (t[1] - '0');
      clock["minute"] = (t[3] - '0') * 10 + (t[4] - '0');
    }
    writable["clockFormat"] = (spaStatusData.clockMode & 0x02) ? "24" : "12";
    writable["tempUnits"] = spaStatusData.tempScale ? "C" : "F";
  }

  JsonObject snapshot = root.createNestedObject("snapshot");
  if (spaInformationData.lastUpdate != 0)
  {
    JsonObject info = snapshot.createNestedObject("information");
    info["softwareID"] = trimField(spaInformationData.softwareID);
    info["model"] = trimField(spaInformationData.model);
    info["configurationSignature"] = informationSignatureHex();
    info["setupNumber"] = spaInformationData.setupNumber;
    info["heaterVoltage"] = spaInformationData.voltage;
    info["heaterType"] = spaInformationData.heaterType;
    info["rawHex"] = rawFrameHexUpper(spaInformationData.rawData, spaInformationData.rawDataLength);
  }
  if (spaConfigurationData.lastUpdate != 0)
  {
    JsonObject cfg = snapshot.createNestedObject("configuration");
    cfg["pump1"] = spaConfigurationData.pump1;
    cfg["pump2"] = spaConfigurationData.pump2;
    cfg["pump3"] = spaConfigurationData.pump3;
    cfg["pump4"] = spaConfigurationData.pump4;
    cfg["pump5"] = spaConfigurationData.pump5;
    cfg["pump6"] = spaConfigurationData.pump6;
    cfg["light1"] = spaConfigurationData.light1;
    cfg["light2"] = spaConfigurationData.light2;
    cfg["rawHex"] = rawFrameHexUpper(spaConfigurationData.rawData, spaConfigurationData.rawDataLength);
  }
  if (spaPreferencesData.lastUpdate != 0)
  {
    JsonObject prefs = snapshot.createNestedObject("preferences");
    prefs["reminders"] = spaPreferencesData.reminders;
    prefs["tempScale"] = spaPreferencesData.tempScale;
    prefs["clockMode"] = spaPreferencesData.clockMode;
    prefs["cleanupCycle"] = spaPreferencesData.cleanupCycle;
    prefs["rawHex"] = rawFrameHexUpper(spaPreferencesData.rawData, spaPreferencesData.rawDataLength);
  }
  if (spaSettings0x04Data.lastUpdate != 0)
  {
    JsonObject s04 = snapshot.createNestedObject("settings0x04");
    s04["rawHex"] = rawFrameHexUpper(spaSettings0x04Data.rawData, spaSettings0x04Data.rawDataLength);
  }
  if (spaFaultLogData.lastUpdate != 0)
  {
    JsonObject fault = snapshot.createNestedObject("faultLog");
    fault["faultCode"] = spaFaultLogData.faultCode;
    fault["faultMessage"] = spaFaultLogData.faultMessage;
    fault["totEntry"] = spaFaultLogData.totEntry;
    fault["currEntry"] = spaFaultLogData.currEntry;
    fault["daysAgo"] = spaFaultLogData.daysAgo;
    fault["hour"] = spaFaultLogData.hour;
    fault["minutes"] = spaFaultLogData.minutes;
    fault["faultLogTime"] = spaFormatFaultLogTime(spaFaultLogData);
    fault["rawHex"] = rawFrameHexUpper(spaFaultLogData.rawData, spaFaultLogData.rawDataLength);
  }
}

bool spaConfigImportFromJson(const JsonDocument &doc, JsonObject report, bool dryRun, bool force)
{
  report["dryRun"] = dryRun;
  report.createNestedArray("warnings");
  report.createNestedArray("applied");
  report.createNestedArray("skipped");
  report.createNestedArray("failed");

  const int schema = doc["schemaVersion"] | 0;
  if (schema != 1)
  {
    report["accepted"] = false;
    report["blocked"] = true;
    appendFailed(report, "schema", "unsupported_schema_version");
    return false;
  }

  appendSkipped(report, "snapshot.configuration", "read_only");
  appendSkipped(report, "snapshot.preferences", "read_only");
  appendSkipped(report, "snapshot.settings0x04", "read_only");
  appendSkipped(report, "snapshot.faultLog", "read_only");

  bool blocked = false;
  JsonObjectConst snapInfo;
  if (doc.containsKey("snapshot"))
  {
    snapInfo = doc["snapshot"]["information"];
  }
  if (!snapInfo.isNull() && spaInformationData.lastUpdate != 0)
  {
    const String fileModel = snapInfo["model"] | "";
    const String fileSig = snapInfo["configurationSignature"] | "";
    const String liveModel = trimField(spaInformationData.model);
    const String liveSig = informationSignatureHex();
    if (fileModel.length() > 0 && liveModel.length() > 0 && !fileModel.equalsIgnoreCase(liveModel))
    {
      report["warnings"].add("model mismatch: file " + fileModel + " vs live " + liveModel);
      if (!force)
      {
        blocked = true;
      }
    }
    if (fileSig.length() > 0 && liveSig.length() > 0 && !fileSig.equalsIgnoreCase(liveSig))
    {
      report["warnings"].add("configurationSignature mismatch: file " + fileSig + " vs live " + liveSig);
      if (!force)
      {
        blocked = true;
      }
    }
  }

  if (blocked)
  {
    report["accepted"] = false;
    report["blocked"] = true;
    appendSkipped(report, "writable", "identity_mismatch_requires_force");
    return false;
  }
  report["blocked"] = false;

  JsonObjectConst writable = doc["writable"];
  if (writable.isNull())
  {
    report["accepted"] = true;
    return true;
  }

  if (writable.containsKey("tempUnits"))
  {
    const char *units = writable["tempUnits"];
    bool celsius = false;
    if (units && (strcasecmp(units, "C") == 0 || strcasecmp(units, "Celsius") == 0))
    {
      celsius = true;
    }
    else if (units && (strcasecmp(units, "F") == 0 || strcasecmp(units, "Fahrenheit") == 0))
    {
      celsius = false;
    }
    else
    {
      appendFailed(report, "tempUnits", "invalid_temp_units_payload");
    }
    if (!dryRun)
    {
      SpaCommandResult r = spaSetTemperatureScale(celsius, SPA_COMMAND_SOURCE_WEB);
      appendApplied(report, "tempUnits", r);
    }
    else
    {
      appendApplied(report, "tempUnits", {true, SPA_COMMAND_ACCEPTED, "dry_run"});
    }
  }

  bool use24 = (spaStatusData.clockMode & 0x02) != 0;
  if (writable.containsKey("clockFormat"))
  {
    const char *fmt = writable["clockFormat"];
    if (fmt && strcmp(fmt, "24") == 0)
    {
      use24 = true;
    }
    else if (fmt && strcmp(fmt, "12") == 0)
    {
      use24 = false;
    }
  }

  uint8_t clockHour = 0;
  uint8_t clockMinute = 0;
  bool haveClock = false;
  if (writable.containsKey("panelClock"))
  {
    JsonObjectConst clock = writable["panelClock"];
    if (!clock.isNull())
    {
      clockHour = clock["hour"] | 0;
      clockMinute = clock["minute"] | 0;
      haveClock = true;
    }
  }

  if (writable.containsKey("clockFormat") || haveClock)
  {
    if (haveClock)
    {
      if (!dryRun)
      {
        SpaCommandResult r = spaSetSpaPanelClockTimeEx(clockHour, clockMinute, use24, SPA_COMMAND_SOURCE_WEB);
        appendApplied(report, "panelClock", r);
        if (writable.containsKey("clockFormat"))
        {
          appendApplied(report, "clockFormat", {r.accepted, r.code, r.reason});
        }
      }
      else
      {
        appendApplied(report, "panelClock", {true, SPA_COMMAND_ACCEPTED, "dry_run"});
        if (writable.containsKey("clockFormat"))
        {
          appendApplied(report, "clockFormat", {true, SPA_COMMAND_ACCEPTED, "dry_run"});
        }
      }
    }
    else if (writable.containsKey("clockFormat") && !dryRun)
    {
      SpaCommandResult r = spaSetSpaPanelClockFormat(use24, SPA_COMMAND_SOURCE_WEB);
      appendApplied(report, "clockFormat", r);
    }
    else if (writable.containsKey("clockFormat"))
    {
      appendApplied(report, "clockFormat", {true, SPA_COMMAND_ACCEPTED, "dry_run"});
    }
  }

  if (writable.containsKey("filter"))
  {
    SpaFilterCycleSettings settings{};
    const char *err = nullptr;
    if (!spaParseFilterCycleJson(writable["filter"], settings, false, &err))
    {
      appendFailed(report, "filter", err ? err : "invalid_filter_payload");
    }
    else if (!dryRun)
    {
      SpaCommandResult r = spaSetFilterCycles(settings, SPA_COMMAND_SOURCE_WEB);
      appendApplied(report, "filter", r);
    }
    else
    {
      appendApplied(report, "filter", {true, SPA_COMMAND_ACCEPTED, "dry_run"});
    }
  }

  report["accepted"] = true;
  return true;
}
