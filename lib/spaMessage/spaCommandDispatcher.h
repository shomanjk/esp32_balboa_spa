#ifndef SPA_COMMAND_DISPATCHER_H
#define SPA_COMMAND_DISPATCHER_H

#include <Arduino.h>

enum SpaCommandSource
{
  SPA_COMMAND_SOURCE_UNKNOWN = 0,
  SPA_COMMAND_SOURCE_WEB = 1,
  SPA_COMMAND_SOURCE_MQTT = 2,
};

enum SpaCommandResultCode
{
  SPA_COMMAND_ACCEPTED = 0,
  SPA_COMMAND_NOT_READY = 1,
  SPA_COMMAND_INVALID_ARGUMENT = 2,
};

struct SpaCommandResult
{
  bool accepted;
  SpaCommandResultCode code;
  const char *reason;
};

bool spaCanAcceptCommands();
SpaCommandResult spaSendToggleCommand(uint8_t itemCode, SpaCommandSource source = SPA_COMMAND_SOURCE_UNKNOWN);
SpaCommandResult spaSetTargetTemperature(float targetTemperature, SpaCommandSource source = SPA_COMMAND_SOURCE_UNKNOWN);

#endif
