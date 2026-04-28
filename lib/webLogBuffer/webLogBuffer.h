#ifndef WEB_LOG_BUFFER_H
#define WEB_LOG_BUFFER_H

#include <Arduino.h>
#include <Print.h>

/** Ring buffer of recent log lines for /api/logs and WebSocket tail. */
void webLogBufferSetup(Print &serialSink);

/** Pass to Log.begin(LOG_LEVEL, &webLogBufferGetLogPrint()). */
Print &webLogBufferGetLogPrint();

/** Monotonic sequence of last committed line (0 if none). */
uint32_t webLogBufferNewestSeq();

/**
 * Build JSON: {"newestSeq":n,"compileMaxLevel":m,"currentLevel":c,"lines":[{"s":seq,"t":"..."},...]}
 * Includes lines with seq > since, oldest first, at most limit (capped).
 */
void webLogBufferBuildJsonSince(uint32_t since, unsigned limit, int currentLevel, String &out);

/**
 * Append JSON array of lines with seq in (since, newest] for WebSocket push.
 * If no lines, out is unchanged.
 */
void webLogBufferAppendJsonDelta(uint32_t since, uint32_t newestExclusive, String &out);

/** All buffered lines as one JSON object (for WS connect history). */
void webLogBufferBuildJsonFull(String &out);

/** JSON object: {"currentLevel":n,"compileMaxLevel":m} */
void webLogBufferBuildJsonLogConfig(int currentLevel, String &out);

#endif
