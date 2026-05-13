#include "faultCapture.h"

#include <cstdarg>
#include <cstring>

#include <esp_system.h>

#if defined(DIAG_FAULT_CAPTURE)
#include <ArduinoJson.h>

#define FAULT_MAGIC 0xFA914CA8u
#define FAULT_LINES 16
#define FAULT_MAX 96

#define FAULT_ABORT_MAGIC 0xA601EE01u
#define FAULT_INGRESS_MAGIC 0xB266DC01u

typedef struct
{
  uint32_t magic;
  uint16_t head;
  uint16_t count;
  char lines[FAULT_LINES][FAULT_MAX];
} FaultCaptureRing;

RTC_NOINIT_ATTR static FaultCaptureRing s_fc;

RTC_NOINIT_ATTR static char s_abortRtc[128];
RTC_NOINIT_ATTR static uint32_t s_abort_magic;

RTC_NOINIT_ATTR static char s_lastIngress[96];
RTC_NOINIT_ATTR static uint32_t s_ingress_magic;

static const char *faultResetReasonName(esp_reset_reason_t r)
{
  switch (r)
  {
  case ESP_RST_UNKNOWN:
    return "UNKNOWN";
  case ESP_RST_POWERON:
    return "POWERON";
  case ESP_RST_EXT:
    return "EXT";
  case ESP_RST_SW:
    return "SW";
  case ESP_RST_PANIC:
    return "PANIC";
  case ESP_RST_INT_WDT:
    return "INT_WDT";
  case ESP_RST_TASK_WDT:
    return "TASK_WDT";
  case ESP_RST_WDT:
    return "WDT";
  case ESP_RST_DEEPSLEEP:
    return "DEEPSLEEP";
  case ESP_RST_BROWNOUT:
    return "BROWNOUT";
  case ESP_RST_SDIO:
    return "SDIO";
  default:
    return "OTHER";
  }
}

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
  if (s_abort_magic == FAULT_ABORT_MAGIC)
  {
    faultCaptureAppendf("[fault] esp_system_abort %s", s_abortRtc);
    s_abort_magic = 0;
    s_abortRtc[0] = '\0';
  }
  const esp_reset_reason_t r = esp_reset_reason();
  if (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT)
  {
    char buf[FAULT_MAX];
    snprintf(buf, sizeof(buf), "[fault] boot after %s", faultResetReasonName(r));
    faultCaptureAppend(buf);
  }
}

void faultCaptureRecordEspSystemAbort(const char *details)
{
  const char *src = details ? details : "(null)";
  size_t i = 0;
  for (; i < 127 && src[i] != '\0'; i++)
  {
    char c = src[i];
    if (c == '\r' || c == '\n')
    {
      c = ' ';
    }
    s_abortRtc[i] = c;
  }
  s_abortRtc[i] = '\0';
  s_abort_magic = FAULT_ABORT_MAGIC;
}

void faultCaptureSetLastBridgeIngress(const char *asciiFrame)
{
  if (asciiFrame == nullptr)
  {
    return;
  }
  size_t i = 0;
  for (; i < 95 && asciiFrame[i] != '\0'; i++)
  {
    char c = asciiFrame[i];
    if (c == '\r' || c == '\n')
    {
      c = ' ';
    }
    s_lastIngress[i] = c;
  }
  s_lastIngress[i] = '\0';
  s_ingress_magic = FAULT_INGRESS_MAGIC;
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
  if (s_fc.count > 0)
  {
    const uint16_t start = static_cast<uint16_t>((s_fc.head + FAULT_LINES - s_fc.count) % FAULT_LINES);
    for (uint16_t i = 0; i < s_fc.count; i++)
    {
      const uint16_t idx = static_cast<uint16_t>((start + i) % FAULT_LINES);
      arr.add(s_fc.lines[idx]);
    }
  }
  if (s_ingress_magic == FAULT_INGRESS_MAGIC)
  {
    root["lastBridgeIngress"] = s_lastIngress;
  }
  else
  {
    root["lastBridgeIngress"] = "";
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
