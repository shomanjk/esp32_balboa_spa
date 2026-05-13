#include "faultCapture.h"

#include <cstdarg>
#include <cstring>

#include <esp_system.h>

#if defined(DIAG_FAULT_CAPTURE)
#include <ArduinoJson.h>

#define FAULT_MAGIC 0xFA914CA8u
#define FAULT_LINES 16
#define FAULT_MAX 96

typedef struct
{
  uint32_t magic;
  uint16_t head;
  uint16_t count;
  char lines[FAULT_LINES][FAULT_MAX];
} FaultCaptureRing;

RTC_NOINIT_ATTR static FaultCaptureRing s_fc;

static void faultCaptureEnsureInit()
{
  if (s_fc.magic != FAULT_MAGIC)
  {
    memset(&s_fc, 0, sizeof(s_fc));
    s_fc.magic = FAULT_MAGIC;
  }
}

void faultCaptureInit()
{
  faultCaptureEnsureInit();
}

void faultCaptureOnBootFromResetReason()
{
  faultCaptureEnsureInit();
  const esp_reset_reason_t r = esp_reset_reason();
  if (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT)
  {
    char buf[FAULT_MAX];
    snprintf(buf, sizeof(buf), "[fault] boot after reset_reason=%d", static_cast<int>(r));
    faultCaptureAppend(buf);
  }
}

void faultCaptureAppend(const char *line)
{
  if (line == nullptr)
  {
    return;
  }
  faultCaptureEnsureInit();
  size_t n = strlen(line);
  if (n >= FAULT_MAX)
  {
    n = FAULT_MAX - 1;
  }
  const uint16_t w = static_cast<uint16_t>(s_fc.head % FAULT_LINES);
  memcpy(s_fc.lines[w], line, n);
  s_fc.lines[w][n] = '\0';
  s_fc.head = static_cast<uint16_t>((s_fc.head + 1) % FAULT_LINES);
  if (s_fc.count < FAULT_LINES)
  {
    s_fc.count++;
  }
}

void faultCaptureAppendf(const char *fmt, ...)
{
  char buf[FAULT_MAX];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  faultCaptureAppend(buf);
}

void faultCaptureAppendToJson(JsonObject root)
{
  faultCaptureEnsureInit();
  JsonArray arr = root.createNestedArray("faultLog");
  if (s_fc.count == 0)
  {
    return;
  }
  const uint16_t start = static_cast<uint16_t>((s_fc.head + FAULT_LINES - s_fc.count) % FAULT_LINES);
  for (uint16_t i = 0; i < s_fc.count; i++)
  {
    const uint16_t idx = static_cast<uint16_t>((start + i) % FAULT_LINES);
    arr.add(s_fc.lines[idx]);
  }
}

#else

void faultCaptureInit() {}
void faultCaptureOnBootFromResetReason() {}
void faultCaptureAppend(const char *) {}
void faultCaptureAppendf(const char *fmt, ...)
{
  (void)fmt;
}

#endif
