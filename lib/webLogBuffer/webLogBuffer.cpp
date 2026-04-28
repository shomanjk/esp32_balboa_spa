#include "webLogBuffer.h"

#include <ArduinoLog.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "../../src/main.h"

#ifndef WEB_LOG_RING_LINES
#define WEB_LOG_RING_LINES 80
#endif
#ifndef WEB_LOG_LINE_MAX
#define WEB_LOG_LINE_MAX 192
#endif

struct WebLogRingLine
{
  uint32_t seq;
  char text[WEB_LOG_LINE_MAX];
};

static WebLogRingLine s_ring[WEB_LOG_RING_LINES];
static uint16_t s_ringWrite = 0;
static uint16_t s_lineCount = 0;
static uint32_t s_nextSeq = 1;
static SemaphoreHandle_t s_mutex;

static Print *s_serial = nullptr;

static void commitLine(const char *buf)
{
  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE)
  {
    return;
  }

  s_ring[s_ringWrite].seq = s_nextSeq++;
  strncpy(s_ring[s_ringWrite].text, buf, WEB_LOG_LINE_MAX - 1);
  s_ring[s_ringWrite].text[WEB_LOG_LINE_MAX - 1] = '\0';
  s_ringWrite = (uint16_t)((s_ringWrite + 1) % WEB_LOG_RING_LINES);
  if (s_lineCount < WEB_LOG_RING_LINES)
  {
    s_lineCount++;
  }

  xSemaphoreGive(s_mutex);
}

class WebLogTee : public Print
{
  char lineBuf[WEB_LOG_LINE_MAX];
  size_t lineLen = 0;

public:
  size_t write(uint8_t c) override
  {
    if (s_serial)
    {
      s_serial->write(c);
    }

    if (c == '\r')
    {
      return 1;
    }
    if (c == '\n')
    {
      lineBuf[lineLen < WEB_LOG_LINE_MAX ? lineLen : WEB_LOG_LINE_MAX - 1] = '\0';
      commitLine(lineBuf);
      lineLen = 0;
      return 1;
    }
    if (lineLen < WEB_LOG_LINE_MAX - 4)
    {
      lineBuf[lineLen++] = (char)c;
    }
    else if (lineLen == WEB_LOG_LINE_MAX - 4)
    {
      lineBuf[lineLen++] = '>';
      lineBuf[lineLen++] = '>';
      lineBuf[lineLen++] = '>';
      lineBuf[lineLen++] = '\0';
    }
    return 1;
  }

  size_t write(const uint8_t *buffer, size_t size) override
  {
    for (size_t i = 0; i < size; i++)
    {
      write(buffer[i]);
    }
    return size;
  }
};

static WebLogTee s_tee;

void webLogBufferSetup(Print &serialSink)
{
  if (s_mutex == nullptr)
  {
    s_mutex = xSemaphoreCreateMutex();
  }
  s_serial = &serialSink;
}

Print &webLogBufferGetLogPrint()
{
  return s_tee;
}

uint32_t webLogBufferNewestSeq()
{
  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE)
  {
    return 0;
  }
  uint32_t n = (s_nextSeq > 0) ? (s_nextSeq - 1) : 0;
  xSemaphoreGive(s_mutex);
  return n;
}

static void appendJsonEscaped(const char *s, String &out)
{
  for (const char *p = s; *p; p++)
  {
    char c = *p;
    switch (c)
    {
    case '\\':
      out += "\\\\";
      break;
    case '"':
      out += "\\\"";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if ((uint8_t)c < 0x20)
      {
        char hx[7];
        snprintf(hx, sizeof(hx), "\\u%04x", (unsigned)c);
        out += hx;
      }
      else
      {
        out += c;
      }
      break;
    }
  }
}

/** Oldest slot index (0..RING-1). */
static uint16_t ringOldestIndex()
{
  if (s_lineCount == 0)
  {
    return 0;
  }
  uint32_t w = s_ringWrite;
  uint32_t c = s_lineCount;
  return (uint16_t)((w + WEB_LOG_RING_LINES - c) % WEB_LOG_RING_LINES);
}

void webLogBufferBuildJsonSince(uint32_t since, unsigned limit, int currentLevel, String &out)
{
  if (limit == 0 || limit > 200)
  {
    limit = 200;
  }

  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
  {
    out = "{\"error\":\"lock_timeout\"}";
    return;
  }

  out = "{\"newestSeq\":";
  out += String((s_nextSeq > 0) ? (s_nextSeq - 1) : 0);
  out += ",\"compileMaxLevel\":";
  out += String((int)LOG_LEVEL);
  out += ",\"currentLevel\":";
  out += String(currentLevel);
  out += ",\"lines\":[";

  bool first = true;
  unsigned emitted = 0;
  for (unsigned i = 0; i < s_lineCount && emitted < limit; i++)
  {
    uint16_t idx = (uint16_t)((ringOldestIndex() + i) % WEB_LOG_RING_LINES);
    if (s_ring[idx].seq > since)
    {
      if (!first)
      {
        out += ',';
      }
      first = false;
      out += "{\"s\":";
      out += String(s_ring[idx].seq);
      out += ",\"t\":\"";
      appendJsonEscaped(s_ring[idx].text, out);
      out += "\"}";
      emitted++;
    }
  }

  out += "]}";
  xSemaphoreGive(s_mutex);
}

void webLogBufferAppendJsonDelta(uint32_t since, uint32_t newestExclusive, String &out)
{
  if (since >= newestExclusive)
  {
    return;
  }

  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE)
  {
    return;
  }

  bool first = true;
  for (unsigned i = 0; i < s_lineCount; i++)
  {
    uint16_t idx = (uint16_t)((ringOldestIndex() + i) % WEB_LOG_RING_LINES);
    uint32_t seq = s_ring[idx].seq;
    if (seq > since && seq <= newestExclusive)
    {
      if (first)
      {
        out += "{\"d\":[";
        first = false;
      }
      else
      {
        out += ',';
      }
      out += "{\"s\":";
      out += String(seq);
      out += ",\"t\":\"";
      appendJsonEscaped(s_ring[idx].text, out);
      out += "\"}";
    }
  }
  if (!first)
  {
    out += "]}";
  }

  xSemaphoreGive(s_mutex);
}

void webLogBufferBuildJsonFull(String &out)
{
  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
  {
    out = "{\"error\":\"lock_timeout\"}";
    return;
  }

  out = "{\"type\":\"history\",\"newestSeq\":";
  out += String((s_nextSeq > 0) ? (s_nextSeq - 1) : 0);
  out += ",\"lines\":[";

  bool first = true;
  for (unsigned i = 0; i < s_lineCount; i++)
  {
    uint16_t idx = (uint16_t)((ringOldestIndex() + i) % WEB_LOG_RING_LINES);
    if (!first)
    {
      out += ',';
    }
    first = false;
    out += "{\"s\":";
    out += String(s_ring[idx].seq);
    out += ",\"t\":\"";
    appendJsonEscaped(s_ring[idx].text, out);
    out += "\"}";
  }

  out += "]}";
  xSemaphoreGive(s_mutex);
}

void webLogBufferBuildJsonLogConfig(int currentLevel, String &out)
{
  out = "{\"currentLevel\":";
  out += String(currentLevel);
  out += ",\"compileMaxLevel\":";
  out += String((int)LOG_LEVEL);
  out += "}";
}
