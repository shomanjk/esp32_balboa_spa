#ifndef SPA_CONFIG_EXPORT_H
#define SPA_CONFIG_EXPORT_H

#include <ArduinoJson.h>
#include <Arduino.h>
#include "spaCommandDispatcher.h"

void spaConfigAppendFilterGetJson(JsonObject root);
void spaConfigAppendPreferencesGetJson(JsonObject root);
void spaConfigAppendFaultLogGetJson(JsonObject root);
void spaConfigAppendFaultLogHistoryGetJson(JsonObject root);
void spaConfigAppendExportJson(JsonObject root);
/** Build import result object into `report`; returns false if schema unsupported or blocked. */
bool spaConfigImportFromJson(const JsonDocument &doc, JsonObject report, bool dryRun, bool force);

#endif
