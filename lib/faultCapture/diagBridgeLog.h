#ifndef DIAG_BRIDGE_LOG_H
#define DIAG_BRIDGE_LOG_H

#include <ArduinoLog.h>

#if defined(DIAG_FAULT_CAPTURE)
/** High-rate `[BridgeDiag]` lines — verbose when fault capture is on so default WARNING stays readable. */
#define BRIDGE_LOG_NOISY(...) Log.verbose(__VA_ARGS__)
#else
#define BRIDGE_LOG_NOISY(...) Log.notice(__VA_ARGS__)
#endif

#endif
