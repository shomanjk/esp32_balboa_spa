#include "spaWebServer.h"

#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <ArduinoLog.h>
#include <webLogBuffer.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <base64.hpp>
#include "FS.h"
#include <LittleFS.h>
#ifdef spaEpaper
#include <epd47.h>
#endif

#define FORMAT_LITTLEFS_IF_FAILED true

// Internal libraries

#include <tinyxml2.h>
#include <cmath>
#include <cstring>
#include <ctime>
#include <memory>
#include <new>
#include <spaMessage.h>
#include <tempHistory.h>
#include <spaUtilities.h>
#include <restartReason.h>
#include <faultCapture.h>
#include <rs485.h>
#include <mqttModule.h>
#include <wifiModule.h>
#include "../../src/config.h"
#include "../../src/main.h"
#include "spaConfigExport.h"

// Local functions

void handleConfig(AsyncWebServerRequest *request);
void handleStatus(AsyncWebServerRequest *request);
void handleState(AsyncWebServerRequest *request);
void handleVersion(AsyncWebServerRequest *request);
void handleDiagnostics(AsyncWebServerRequest *request);
void handleWifi(AsyncWebServerRequest *request);
void handleMqtt(AsyncWebServerRequest *request);
void handleStatusControlsApi(AsyncWebServerRequest *request);
void handleStatusSummaryApi(AsyncWebServerRequest *request);
void handleStatusHistoriesApi(AsyncWebServerRequest *request);
void handleStateLittleFsApi(AsyncWebServerRequest *request);
void handleDiagToggleApi(AsyncWebServerRequest *request);
void handleDiagToggleSequenceApi(AsyncWebServerRequest *request);
void handleDiagLight1NextCtsApi(AsyncWebServerRequest *request);
void handleDiagLight1NextCtsWindowApi(AsyncWebServerRequest *request);
void handleRs485(AsyncWebServerRequest *request);
void handleRs485Retry(AsyncWebServerRequest *request);
void handleRs485Raw(AsyncWebServerRequest *request);
void handleRs485History(AsyncWebServerRequest *request);
void handleSlash(AsyncWebServerRequest *request);
void handleNotFound(AsyncWebServerRequest *request);
void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total);
void handleData(AsyncWebServerRequest *request);
void handleLoginData(AsyncWebServerRequest *request);
void handleOptionsData(AsyncWebServerRequest *request);
void handleOptionsLoginData(AsyncWebServerRequest *request);
void handleepdpanel(AsyncWebServerRequest *request);
void handleLogsApi(AsyncWebServerRequest *request);
void handleLogsPage(AsyncWebServerRequest *request);
void handleLogsConfigGet(AsyncWebServerRequest *request);
void handleLogsConfigPost(AsyncWebServerRequest *request);
void handleConfigFilterGet(AsyncWebServerRequest *request);
void handleConfigFilterPost(AsyncWebServerRequest *request);
void handleConfigPreferencesGet(AsyncWebServerRequest *request);
void handleConfigPreferencesPost(AsyncWebServerRequest *request);
void handleConfigFaultLogGet(AsyncWebServerRequest *request);
void handleConfigFaultLogHistoryGet(AsyncWebServerRequest *request);
void handleConfigFaultLogHistoryPost(AsyncWebServerRequest *request);
void handleConfigExport(AsyncWebServerRequest *request);
void handleConfigImportPost(AsyncWebServerRequest *request);
String parseBody(String body);
void listDir(fs::FS &fs, const char *dirname, uint8_t levels);
String listDirToString(fs::FS &fs, const char *dirname, uint8_t levels);

/** Contiguous heap needed for a small Async*Response + a couple of addHeader Strings. */
static constexpr size_t kPortalHtmlResponseHeadroom = 2048;
/** ESPAsyncWebServer/AsyncTCP can stall browser transfers when a callback fills >2 KiB at once. */
static constexpr size_t kPortalResponseFillMax = 2048;

#ifndef HTTP_LIVENESS_PROBE_INTERVAL_MS
#define HTTP_LIVENESS_PROBE_INTERVAL_MS (2UL * 60UL * 1000UL)
#endif
#ifndef HTTP_LIVENESS_PROBE_TIMEOUT_MS
#define HTTP_LIVENESS_PROBE_TIMEOUT_MS 5000UL
#endif
#ifndef HTTP_LIVENESS_PROBE_FAIL_MAX
#define HTTP_LIVENESS_PROBE_FAIL_MAX 3
#endif
#ifndef HTTP_LIVENESS_MIN_UPTIME_MS
#define HTTP_LIVENESS_MIN_UPTIME_MS (3UL * 60UL * 1000UL)
#endif
#ifndef HTTP_LIVENESS_WATCHDOG
#define HTTP_LIVENESS_WATCHDOG 1
#endif

static volatile uint8_t s_portalLargePageInFlight = 0;
static unsigned long s_lastHttpProbeMs = 0;
static uint8_t s_httpProbeFailStreak = 0;
extern bool serverSetup;

using SpaWebHandlerFn = void (*)(AsyncWebServerRequest *);

static void spaWebTouchHttpActivity()
{
  /* Reserved for future diagnostics; liveness uses loopback probes, not idle time. */
}

static void spaWebDispatch(SpaWebHandlerFn handler, AsyncWebServerRequest *request)
{
  spaWebTouchHttpActivity();
  handler(request);
}

static void portalLargePageRelease()
{
  if (s_portalLargePageInFlight > 0)
  {
    s_portalLargePageInFlight = 0;
  }
}

/** Gate /status, /config, /state, /logs assembly — one large portal page at a time. */
/** Largest block plain malloc/new can actually get. ESP.getMaxAllocHeap()/getFreeHeap() also
 *  count a ~40KB 32-bit-only IRAM region normal allocations can never use, which made every
 *  "low memory" log/guard here lie by that margin. */
static inline uint32_t portalUsableLargestBlock()
{
  return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
}

/** 503 for transient assembly OOM (page + SPA hydration overlapped): a tiny PROGMEM page that
 *  retries itself in 2s, when the parallel JSON responses have drained. Plain-text dead ends
 *  made users think the device was broken. */
static const char kPortalRetryHtml[] PROGMEM =
    "<!DOCTYPE html><html><head><meta charset='utf-8'><meta http-equiv='refresh' content='2'>"
    "<title>Spa portal busy</title></head><body style='font-family:sans-serif'>"
    "<p>The portal is momentarily out of memory (a page build and app refresh overlapped).</p>"
    "<p>Retrying automatically&hellip;</p></body></html>";

static void sendPortalBusyRetry(AsyncWebServerRequest *request)
{
  AsyncWebServerResponse *response = request->beginResponse_P(
      503, "text/html", reinterpret_cast<const uint8_t *>(kPortalRetryHtml), sizeof(kPortalRetryHtml) - 1);
  if (response != nullptr)
  {
    response->addHeader("Retry-After", "2");
    request->send(response);
    return;
  }
  request->send(503, "text/plain", "portal busy, retry shortly");
}

static bool portalLargePageTryBegin(AsyncWebServerRequest *request, const char *pageTag)
{
  if (s_portalLargePageInFlight != 0)
  {
    Log.warning("[Web]: %s rejected — portal page busy" CR, pageTag);
    spaWebTouchHttpActivity();
    request->send(503, "text/plain", "portal page busy (try again shortly)");
    return false;
  }
  s_portalLargePageInFlight = 1;
  return true;
}

static void spaWebHttpLivenessTick();

/** Loopback GET /api/version — false when the local web stack cannot serve HTTP. */
static bool spaWebHttpLivenessProbeOnce()
{
  const IPAddress ip = WiFi.localIP();
  if (ip == IPAddress(0, 0, 0, 0))
  {
    return true;
  }

  WiFiClient client;
  client.setTimeout(static_cast<uint16_t>((HTTP_LIVENESS_PROBE_TIMEOUT_MS + 999UL) / 1000UL));
  if (!client.connect(ip, 80))
  {
    Log.warning(F("[Web]: HTTP liveness probe connect failed" CR));
    return false;
  }

  client.print(F("GET /api/version HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));

  const unsigned long deadlineMs = millis() + HTTP_LIVENESS_PROBE_TIMEOUT_MS;
  char lineBuf[64];
  size_t lineLen = 0;
  bool saw200 = false;

  while (millis() < deadlineMs && !saw200)
  {
    while (client.available())
    {
      const int ch = client.read();
      if (ch < 0)
      {
        break;
      }
      if (ch == '\n')
      {
        lineBuf[lineLen] = '\0';
        if (strstr(lineBuf, "200") != nullptr)
        {
          saw200 = true;
          break;
        }
        lineLen = 0;
      }
      else if (ch != '\r' && lineLen + 1 < sizeof(lineBuf))
      {
        lineBuf[lineLen++] = static_cast<char>(ch);
      }
    }
    if (!client.connected() && !client.available())
    {
      break;
    }
    delay(1);
  }
  client.stop();
  if (!saw200)
  {
    Log.warning(F("[Web]: HTTP liveness probe missing HTTP 200" CR));
  }
  return saw200;
}

static bool ifNoneMatchHits(AsyncWebServerRequest *request, const String &etag)
{
  if (!request || etag.length() == 0 || !request->hasHeader("If-None-Match"))
  {
    return false;
  }
  const AsyncWebHeader *matchHeader = request->getHeader("If-None-Match");
  if (!matchHeader)
  {
    return false;
  }
  // Header value copy is small; avoid holding large page bodies while allocating the 304.
  const String matchValue = matchHeader->value();
  return matchValue.indexOf(etag) >= 0;
}

/**
 * Send HTTP 304 with nothrow response alloc. Caller must free large page bodies first so slabs
 * are not still consuming heap under -fno-exceptions. Returns true if 304 was queued; false if
 * a 503 was sent instead.
 */
static bool sendNotModified304Soft(AsyncWebServerRequest *request, const String &etag)
{
  const uint32_t maxAlloc = portalUsableLargestBlock();
  if (maxAlloc < kPortalHtmlResponseHeadroom)
  {
    Log.error("[Web]: portal HTML 304 headroom low maxAlloc=%u need=%u freeHeap=%u" CR,
              static_cast<unsigned>(maxAlloc), static_cast<unsigned>(kPortalHtmlResponseHeadroom),
              static_cast<unsigned>(ESP.getFreeHeap()));
    spaWebTouchHttpActivity();
    sendPortalBusyRetry(request);
    portalLargePageRelease();
    return false;
  }

  AsyncBasicResponse *notModified = new (std::nothrow) AsyncBasicResponse(304);
  if (notModified == nullptr)
  {
    Log.error("[Web]: portal HTML 304 alloc failed freeHeap=%u maxAlloc=%u" CR,
              static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
    spaWebTouchHttpActivity();
    sendPortalBusyRetry(request);
    portalLargePageRelease();
    return false;
  }
  notModified->addHeader("ETag", etag);
  notModified->addHeader("Cache-Control", "no-cache");
  request->send(notModified);
  spaWebTouchHttpActivity();
  portalLargePageRelease();
  return true;
}

/** Send HTML with ETag. Uses callback body delivery so ESPAsyncWebServer does not keep a second
 *  full copy of the page in `AsyncBasicResponse::_content` (which can peak at ~2–3× RAM for
 *  large `/status` and reset TCP mid-transfer on ESP32). */
static void sendHtmlWithEtag(AsyncWebServerRequest *request, String &html, const String &etag,
                             bool releasePortalGateOnDisconnect = false)
{
  if (!request)
  {
    if (releasePortalGateOnDisconnect)
    {
      portalLargePageRelease();
    }
    return;
  }
  if (ifNoneMatchHits(request, etag))
  {
    html = String();
    (void)sendNotModified304Soft(request, etag);
    return;
  }
  auto sharedBody = std::make_shared<String>(std::move(html));
  if (releasePortalGateOnDisconnect)
  {
    request->onDisconnect([]() {
      portalLargePageRelease();
    });
  }
  AsyncWebServerResponse *response = request->beginChunkedResponse(
      "text/html; charset=utf-8",
      [sharedBody](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
        if (index >= sharedBody->length())
        {
          return 0;
        }
        size_t n = sharedBody->length() - index;
        if (n > maxLen)
        {
          n = maxLen;
        }
        if (n > kPortalResponseFillMax)
        {
          n = kPortalResponseFillMax;
        }
        memcpy(buffer, sharedBody->c_str() + index, n);
        return n;
      });
  response->addHeader("ETag", etag);
  response->addHeader("Cache-Control", "no-cache");
  request->send(response);
  spaWebTouchHttpActivity();
}

/**
 * Assemble large portal HTML as small RAM slabs so Arduino `String` never needs one contiguous
 * ~70–90KB heap block (Atom Lite has no PSRAM; fragmentation truncates mid-document). Streamed
 * with the same ETag callback pattern as `sendHtmlWithEtag`. No LittleFS writes.
 *
 * Fixed slab table (not std::vector) so metadata growth cannot throw/abort under -fno-exceptions;
 * only nothrow payload allocs feed the healthy_/503 path.
 */
struct PortalHtmlChunkBody
{
  static constexpr size_t kMaxSlabs = 40; // 40 × 4 KiB = 160 KiB max page

  std::unique_ptr<uint8_t[]> slabs[kMaxSlabs];
  size_t slabLens[kMaxSlabs] = {};
  size_t usedSlabs = 0;
  size_t totalLen = 0;
};

/** Single nothrow malloc for delivery state (no std::shared_ptr control block). */
struct PortalHtmlChunkHold
{
  PortalHtmlChunkBody body;

  static PortalHtmlChunkHold *create()
  {
    void *mem = ::operator new(sizeof(PortalHtmlChunkHold), std::nothrow);
    if (mem == nullptr)
    {
      return nullptr;
    }
    return new (mem) PortalHtmlChunkHold();
  }

  void destroy()
  {
    this->~PortalHtmlChunkHold();
    ::operator delete(this, std::nothrow);
  }
};

/**
 * AwsResponseFiller is std::function; libstdc++ SBO requires a trivially-copyable callable.
 * Capture only a slot index (POD). Holds live in this table until request disconnect.
 */
static constexpr size_t kPortalHtmlHoldSlots = 4;
static PortalHtmlChunkHold *s_portalHtmlHoldSlots[kPortalHtmlHoldSlots] = {};

static int portalHtmlHoldAcquire(PortalHtmlChunkHold *hold)
{
  if (hold == nullptr)
  {
    return -1;
  }
  for (size_t i = 0; i < kPortalHtmlHoldSlots; i++)
  {
    if (s_portalHtmlHoldSlots[i] == nullptr)
    {
      s_portalHtmlHoldSlots[i] = hold;
      return static_cast<int>(i);
    }
  }
  return -1;
}

static void portalHtmlHoldReleaseSlot(uint8_t slot)
{
  if (slot >= kPortalHtmlHoldSlots)
  {
    return;
  }
  PortalHtmlChunkHold *hold = s_portalHtmlHoldSlots[slot];
  if (hold == nullptr)
  {
    return;
  }
  s_portalHtmlHoldSlots[slot] = nullptr;
  hold->destroy();
}

/** Stream one chunk from a hold slot. Kept free of captures so callers can wrap a POD slot index. */
static size_t portalHtmlChunkFill(uint8_t slotU8, uint8_t *buffer, size_t maxLen, size_t index)
{
  PortalHtmlChunkHold *slotHold = s_portalHtmlHoldSlots[slotU8];
  if (slotHold == nullptr)
  {
    return 0;
  }
  PortalHtmlChunkBody &body = slotHold->body;
  if (index >= body.totalLen || maxLen == 0 || body.usedSlabs == 0)
  {
    return 0;
  }

  // Derive the source position solely from the callback's authoritative index. AsyncTCP may
  // re-enter response filling; shared mutable cursor state can then skip or duplicate a slab.
  size_t slabIndex = 0;
  size_t slabOffset = index;
  while (slabIndex < body.usedSlabs && slabOffset >= body.slabLens[slabIndex])
  {
    slabOffset -= body.slabLens[slabIndex];
    slabIndex++;
  }

  const size_t fillLimit = maxLen < kPortalResponseFillMax ? maxLen : kPortalResponseFillMax;
  size_t written = 0;
  while (written < fillLimit && index + written < body.totalLen)
  {
    if (slabIndex >= body.usedSlabs)
    {
      break;
    }
    const size_t slabLen = body.slabLens[slabIndex];
    if (slabOffset >= slabLen)
    {
      slabIndex++;
      slabOffset = 0;
      continue;
    }
    const size_t avail = slabLen - slabOffset;
    size_t n = fillLimit - written;
    if (n > avail)
    {
      n = avail;
    }
    memcpy(buffer + written, body.slabs[slabIndex].get() + slabOffset, n);
    written += n;
    slabOffset += n;
    if (slabOffset >= slabLen)
    {
      slabIndex++;
      slabOffset = 0;
    }
  }
  return written;
}

class PortalHtmlChunks
{
public:
  static constexpr size_t kMaxSlabs = PortalHtmlChunkBody::kMaxSlabs;

  void clear()
  {
    for (size_t i = 0; i < usedSlabs_; i++)
    {
      slabs_[i].reset();
      slabLens_[i] = 0;
    }
    usedSlabs_ = 0;
    totalLen_ = 0;
    healthy_ = true;
  }

  void reserve(size_t)
  {
    // Per-slab growth; ignore large contiguous reserves from page builders.
  }

  PortalHtmlChunks &operator=(const __FlashStringHelper *s)
  {
    clear();
    return (*this += s);
  }

  PortalHtmlChunks &operator+=(const char *s)
  {
    if (s == nullptr)
    {
      return *this;
    }
    return appendBytes(s, strlen(s));
  }

  PortalHtmlChunks &operator+=(const String &s)
  {
    return appendBytes(s.c_str(), s.length());
  }

  PortalHtmlChunks &operator+=(const __FlashStringHelper *s)
  {
    if (!healthy_ || s == nullptr)
    {
      return *this;
    }
    PGM_P p = reinterpret_cast<PGM_P>(s);
    char tmp[128];
    while (true)
    {
      size_t i = 0;
      while (i < sizeof(tmp) - 1)
      {
        const char c = static_cast<char>(pgm_read_byte(p++));
        if (c == '\0')
        {
          if (i > 0)
          {
            appendBytes(tmp, i);
          }
          return *this;
        }
        tmp[i++] = c;
      }
      appendBytes(tmp, i);
    }
  }

  PortalHtmlChunks &operator+=(char c)
  {
    return appendBytes(&c, 1);
  }

  size_t length() const
  {
    return totalLen_;
  }

  bool healthy() const
  {
    return healthy_;
  }

  size_t slabCount() const
  {
    return usedSlabs_;
  }

  void releaseTo(PortalHtmlChunkBody &out)
  {
    for (size_t i = 0; i < usedSlabs_; i++)
    {
      out.slabs[i] = std::move(slabs_[i]);
      out.slabLens[i] = slabLens_[i];
    }
    out.usedSlabs = usedSlabs_;
    out.totalLen = totalLen_;
    usedSlabs_ = 0;
    totalLen_ = 0;
    healthy_ = true;
  }

private:
  static constexpr size_t kSlabCap = 4096;

  bool pushSlab()
  {
    if (usedSlabs_ >= kMaxSlabs)
    {
      healthy_ = false;
      Log.error("[Web]: portal HTML slab count exhausted (%u) freeHeap=%u maxAlloc=%u" CR,
                static_cast<unsigned>(kMaxSlabs), static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getMaxAllocHeap()));
      return false;
    }
    // A slab alloc can fail transiently while parallel JSON API responses (SPA hydration after
    // a reload) momentarily hold the heap: observed on the bench as a 4 KiB nothrow failure
    // logged alongside maxAlloc=42996 — the memory was back microseconds later. Ride out the
    // contention with brief yielding retries before declaring the page unbuildable.
    std::unique_ptr<uint8_t[]> slab;
    for (int attempt = 0; attempt < 4; attempt++)
    {
      slab.reset(new (std::nothrow) uint8_t[kSlabCap]);
      if (slab)
      {
        break;
      }
      delay(1);
    }
    if (!slab)
    {
      healthy_ = false;
      multi_heap_info_t info8;
      heap_caps_get_info(&info8, MALLOC_CAP_8BIT);
      const bool intact = heap_caps_check_integrity_all(false);
      Log.error("[Web]: portal HTML slab alloc failed after retries freeHeap=%u maxAlloc=%u "
                "cap8free=%u cap8largest=%u cap8blocks=%u integrity=%d" CR,
                static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getMaxAllocHeap()),
                static_cast<unsigned>(info8.total_free_bytes),
                static_cast<unsigned>(info8.largest_free_block),
                static_cast<unsigned>(info8.free_blocks), static_cast<int>(intact));
      return false;
    }
    slabs_[usedSlabs_] = std::move(slab);
    slabLens_[usedSlabs_] = 0;
    usedSlabs_++;
    return true;
  }

  PortalHtmlChunks &appendBytes(const char *p, size_t n)
  {
    if (!healthy_ || p == nullptr || n == 0)
    {
      return *this;
    }
    size_t off = 0;
    while (off < n)
    {
      if (usedSlabs_ == 0 || slabLens_[usedSlabs_ - 1] >= kSlabCap)
      {
        if (!pushSlab())
        {
          return *this;
        }
      }
      const size_t idx = usedSlabs_ - 1;
      const size_t room = kSlabCap - slabLens_[idx];
      size_t take = n - off;
      if (take > room)
      {
        take = room;
      }
      memcpy(slabs_[idx].get() + slabLens_[idx], p + off, take);
      slabLens_[idx] += take;
      totalLen_ += take;
      off += take;
    }
    return *this;
  }

  std::unique_ptr<uint8_t[]> slabs_[kMaxSlabs];
  size_t slabLens_[kMaxSlabs] = {};
  size_t usedSlabs_ = 0;
  size_t totalLen_ = 0;
  bool healthy_ = true;
};

static void sendHtmlChunksWithEtag(AsyncWebServerRequest *request, PortalHtmlChunks &html, const String &etag)
{
  if (!request)
  {
    portalLargePageRelease();
    return;
  }
  if (!html.healthy())
  {
    Log.error("[Web]: portal HTML assemble unhealthy len=%u slabs=%u freeHeap=%u maxAlloc=%u" CR,
              static_cast<unsigned>(html.length()), static_cast<unsigned>(html.slabCount()),
              static_cast<unsigned>(ESP.getFreeHeap()),
              static_cast<unsigned>(ESP.getMaxAllocHeap()));
    html.clear();
    spaWebTouchHttpActivity();
    sendPortalBusyRetry(request);
    portalLargePageRelease();
    return;
  }
  if (ifNoneMatchHits(request, etag))
  {
    // Free slabs before allocating the 304 response (throwing beginResponse(304) raced slabs).
    html.clear();
    (void)sendNotModified304Soft(request, etag);
    return;
  }

  // Delivery hold: one nothrow alloc. Callback/onDisconnect capture only a POD slot index so
  // AwsResponseFiller / ArDisconnectHandler (std::function) stay in libstdc++ SBO — a nontrivial
  // capture (HoldPtr/shared_ptr) forces throwing operator new under -fno-exceptions.
  PortalHtmlChunkHold *hold = PortalHtmlChunkHold::create();
  if (hold == nullptr)
  {
    Log.error("[Web]: portal HTML body hold alloc failed len=%u slabs=%u freeHeap=%u maxAlloc=%u" CR,
              static_cast<unsigned>(html.length()), static_cast<unsigned>(html.slabCount()),
              static_cast<unsigned>(ESP.getFreeHeap()),
              static_cast<unsigned>(ESP.getMaxAllocHeap()));
    html.clear();
    spaWebTouchHttpActivity();
    sendPortalBusyRetry(request);
    portalLargePageRelease();
    return;
  }
  html.releaseTo(hold->body);
  const int slot = portalHtmlHoldAcquire(hold);
  if (slot < 0)
  {
    Log.error("[Web]: portal HTML hold slots exhausted freeHeap=%u" CR,
              static_cast<unsigned>(ESP.getFreeHeap()));
    hold->destroy();
    spaWebTouchHttpActivity();
    sendPortalBusyRetry(request);
    portalLargePageRelease();
    return;
  }
  const uint8_t slotU8 = static_cast<uint8_t>(slot);
  // The request helper uses throwing new. After slabs + hold have consumed heap, require
  // headroom and construct the chunked response with nothrow so this path can 503 instead.
  const uint32_t maxAlloc = portalUsableLargestBlock();
  if (maxAlloc < kPortalHtmlResponseHeadroom)
  {
    Log.error("[Web]: portal HTML response headroom low maxAlloc=%u need=%u freeHeap=%u" CR,
              static_cast<unsigned>(maxAlloc), static_cast<unsigned>(kPortalHtmlResponseHeadroom),
              static_cast<unsigned>(ESP.getFreeHeap()));
    portalHtmlHoldReleaseSlot(slotU8);
    spaWebTouchHttpActivity();
    sendPortalBusyRetry(request);
    portalLargePageRelease();
    return;
  }

  AsyncChunkedResponse *response = new (std::nothrow) AsyncChunkedResponse(
      "text/html; charset=utf-8",
      [slotU8](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
        return portalHtmlChunkFill(slotU8, buffer, maxLen, index);
      });
  if (response == nullptr)
  {
    Log.error("[Web]: portal HTML AsyncChunkedResponse alloc failed freeHeap=%u maxAlloc=%u" CR,
              static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
    portalHtmlHoldReleaseSlot(slotU8);
    spaWebTouchHttpActivity();
    sendPortalBusyRetry(request);
    portalLargePageRelease();
    return;
  }

  // Idempotent free when the TCP client drops (request teardown); POD capture for SBO.
  request->onDisconnect([slotU8]() {
    portalHtmlHoldReleaseSlot(slotU8);
    portalLargePageRelease();
  });
  response->addHeader("ETag", etag);
  response->addHeader("Cache-Control", "no-cache");
  request->send(response);
  spaWebTouchHttpActivity();
}

static const char *wifiStatusName(wl_status_t s)
{
  switch (s)
  {
  case WL_IDLE_STATUS:
    return "Idle";
  case WL_NO_SSID_AVAIL:
    return "No SSID";
  case WL_SCAN_COMPLETED:
    return "Scan completed";
  case WL_CONNECTED:
    return "Connected";
  case WL_CONNECT_FAILED:
    return "Connect failed";
  case WL_CONNECTION_LOST:
    return "Connection lost";
  case WL_DISCONNECTED:
    return "Disconnected";
  default:
    return "Unknown";
  }
}

static const char *mqttStateName(int s)
{
  switch (s)
  {
  case 0:
    return "Connected";
  case -1:
    return "Disconnected";
  case -2:
    return "Connect failed";
  case -3:
    return "Connection lost";
  case -4:
    return "Timeout";
  case -5:
    return "Bad credentials";
  default:
    return "Unknown";
  }
}

static String rs485HealthLabel(const String &health)
{
  if (health == "VALID_FRAMES_OK")
  {
    return "Healthy";
  }
  if (health == "UART_BYTES_NO_VALID_FRAMES")
  {
    return "Bytes seen, no valid frames";
  }
  if (health == "UART_DEFERRED")
  {
    return "UART deferred (waiting for Wi‑Fi/OTA)";
  }
  if (health == "RS485_SAFE_MODE")
  {
    return "RS485 safe mode";
  }
  return "No UART bytes";
}

static String rs485HealthColor(const String &health)
{
  if (health == "VALID_FRAMES_OK")
  {
    return "#04AA6D";
  }
  if (health == "UART_BYTES_NO_VALID_FRAMES")
  {
    return "#ef6c00";
  }
  if (health == "UART_DEFERRED")
  {
    return "#546e7a";
  }
  if (health == "RS485_SAFE_MODE")
  {
    return "#ef6c00";
  }
  return "#c62828";
}

static String rs485ModeHint(bool inverted)
{
  return inverted
             ? "Inverted RX/TX mode is active (virtual A/B swap)."
             : "Normal RX/TX mode is active.";
}

static String rs485HealthHint(const String &health)
{
  if (health == "VALID_FRAMES_OK")
  {
    return "Balboa frames are decoding correctly.";
  }
  if (health == "UART_BYTES_NO_VALID_FRAMES")
  {
    return "Signal is present, but framing/quality is not yet valid.";
  }
  if (health == "UART_DEFERRED")
  {
    return "RS485 UART starts only after Wi‑Fi is up and ArduinoOTA is listening.";
  }
  if (health == "RS485_SAFE_MODE")
  {
    return "UART is skipped after repeated faults so OTA stays reachable. Fix pins, then POST /api/rs485/retry or power-cycle.";
  }
  return "No bus activity detected at UART RX yet.";
}

struct GatewayChipTempStatus
{
  bool available;
  float tempC;
  const char *statusKey;
  const char *statusLabel;
  const char *badgeColor;
};

static bool readGatewayChipTempC(float &outC)
{
  const float tempC = temperatureRead();
  if (std::isnan(tempC))
  {
    return false;
  }
  outC = tempC;
  return true;
}

static GatewayChipTempStatus classifyGatewayChipTemp(float tempC)
{
  GatewayChipTempStatus status;
  status.available = true;
  status.tempC = tempC;
  if (tempC >= 100.0f)
  {
    status.statusKey = "critical";
    status.statusLabel = "Critical";
    status.badgeColor = "#c62828";
  }
  else if (tempC >= 85.0f)
  {
    status.statusKey = "high";
    status.statusLabel = "High";
    status.badgeColor = "#c62828";
  }
  else if (tempC >= 75.0f)
  {
    status.statusKey = "elevated";
    status.statusLabel = "Elevated";
    status.badgeColor = "#ef6c00";
  }
  else
  {
    status.statusKey = "normal";
    status.statusLabel = "Normal";
    status.badgeColor = "#04AA6D";
  }
  return status;
}

static GatewayChipTempStatus gatewayChipTempSnapshot()
{
  float tempC = 0.0f;
  if (!readGatewayChipTempC(tempC))
  {
    GatewayChipTempStatus status;
    status.available = false;
    status.tempC = 0.0f;
    status.statusKey = "unavailable";
    status.statusLabel = "Unavailable";
    status.badgeColor = "#4b5563";
    return status;
  }
  return classifyGatewayChipTemp(tempC);
}

static void appendGatewayChipTempJson(JsonDocument &doc)
{
  const GatewayChipTempStatus chip = gatewayChipTempSnapshot();
  doc["chipTempAvailable"] = chip.available;
  doc["chipTempStatus"] = chip.statusKey;
  doc["chipTempStatusLabel"] = chip.statusLabel;
  if (chip.available)
  {
    doc["chipTempC"] = roundf(chip.tempC * 10.0f) / 10.0f;
  }
}

template <typename HtmlOut>
static void appendGatewayChipTempStateSubCard(HtmlOut &html)
{
  const GatewayChipTempStatus chip = gatewayChipTempSnapshot();
  html += "<div class='sub-card'><p class='sub-card-title'>Chip temperature</p>";
  html += "<div style='margin-bottom:8px'><span class='diag-badge' style='font-weight:700;color:#fff;background:";
  html += chip.badgeColor;
  html += "'>";
  html += chip.statusLabel;
  html += "</span></div><div class='sub-card-row'><b>Die sensor: </b><span>";
  if (chip.available)
  {
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f °C", chip.tempC);
    html += buf;
  }
  else
  {
    html += "—";
  }
  html += "</span></div>";
  html += "<p style='margin:8px 0 0 0;font-size:14px;color:var(--muted)'>ESP32 die sensor — approximate; not cabinet ambient.</p></div>";
}

template <typename HtmlOut>
static void appendWifiStateSection(HtmlOut &html)
{
  wl_status_t st = WiFi.status();
  bool ok = (st == WL_CONNECTED);
  const char *statusName = wifiStatusName(st);
  String badgeBg = ok ? String("#04AA6D") : (st == WL_IDLE_STATUS ? String("#4b5563") : String("#c62828"));
  if (!ok && (st == WL_CONNECT_FAILED || st == WL_CONNECTION_LOST))
  {
    badgeBg = "#ef6c00";
  }

  html += "<section class='panel'><h1>WiFi</h1>";
  html += "<div class='wifi-hero'><div class='wifi-hero__status'><span id=\"wf-status-badge\" class='diag-badge' style='font-weight:700;color:#fff;background:";
  html += badgeBg;
  html += "'>";
  html += statusName;
  html += "</span></div><div class='wifi-hero__net'><div class='wifi-hero__ssid' id=\"wf-ssid\">";
  html += ok ? WiFi.SSID() : String("—");
  html += "</div><div class='wifi-hero__host' id=\"wf-host\">";
  html += WiFi.getHostname() ? String(WiFi.getHostname()) : String("—");
  html += "</div></div><div class='wifi-hero__signal'><div class='wifi-hero__rssi'><span id=\"wf-rssi\">";
  html += ok ? String(WiFi.RSSI()) + " dBm" : String("—");
  html += "</span></div><div id=\"wf-quality\" style=\"font-size:12px\"></div></div></div>";
  html += "<p class='wifi-meta'>Ch <span id=\"wf-ch\">";
  html += ok ? String(WiFi.channel()) : String("—");
  html += "</span><span class='wifi-meta__sep'>&middot;</span>STA MAC <span id=\"wf-mac\">";
  html += WiFi.macAddress();
  html += "</span><span class='wifi-meta__sep'>&middot;</span>AP BSSID <span id=\"wf-bssid\">";
  html += ok ? WiFi.BSSIDstr() : String("—");
  html += "</span><span class='wifi-meta__sep'>&middot;</span>Status code <span id=\"wf-st\">";
  html += String(static_cast<int>(st));
  html += "</span></p>";
  html += "<div class='wifi-body'><div class='wifi-network'><p class='wifi-block-title'>Network</p><dl class='wifi-kv'>";
  html += "<dt>IP</dt><dd id=\"wf-ip\">";
  html += ok ? WiFi.localIP().toString() : String("—");
  html += "</dd><dt>Gateway</dt><dd id=\"wf-gw\">";
  html += ok ? WiFi.gatewayIP().toString() : String("—");
  html += "</dd><dt>Subnet</dt><dd id=\"wf-sn\">";
  html += ok ? WiFi.subnetMask().toString() : String("—");
  html += "</dd><dt>DNS</dt><dd id=\"wf-dns\">";
  html += ok ? WiFi.dnsIP(0).toString() : String("—");
  html += "</dd><dt>BSSID lock</dt><dd id=\"wf-bssid-lock\">";
  if (wifiBssidLockActive() && wifiConfiguredBssidLock()[0] != '\0')
  {
    html += wifiConfiguredBssidLock();
  }
  else
  {
    html += "— (strongest AP)";
  }
  html += "</dd></dl></div>";
  html += "<div class='wifi-signal-card'><p class='wifi-block-title'>Signal</p>";
  html += "<div class='wifi-signal-row'><span class='wifi-signal-row__label'>Now</span><span class='wifi-signal-row__value' id=\"wf-signal-now\">";
  html += ok ? String(WiFi.RSSI()) + " dBm" : String("—");
  html += "</span></div>";
  html += "<div class='wifi-signal-row'><span class='wifi-signal-row__label'>5 min avg</span><span class='wifi-signal-row__value' id=\"wf-avg\">—</span></div>";
  html += "<p class='wifi-signal-caption'>RSSI over time (5s samples, ~5 min)</p>";
  html += "<div class='chart-wrap'><canvas id=\"wifiRssiChart\" height=\"120\"></canvas></div></div></div>";
  html += "<script>";
  html += "(function(){var pollMs=5000,maxPts=60,warnRssi=-75,badRssi=-80,chartH=120;var c=document.getElementById('wifiRssiChart');";
  html += "if(!c)return;var x=c.getContext('2d'),d=[];";
  html += "function set(t,v){var e=document.getElementById(t);if(e)e.textContent=v;}";
  html += "function colorOf(v){if(v<=badRssi)return '#c62828';if(v<=warnRssi)return '#ef6c00';return '#04AA6D';}";
  html += "function qualityOf(v){if(v<=badRssi)return 'Weak';if(v<=warnRssi)return 'Fair';if(v<=-67)return 'Good';return 'Excellent';}";
  html += "function badgeBg(connected,status){if(connected)return '#04AA6D';if(status===0)return '#4b5563';if(status===4||status===5)return '#ef6c00';return '#c62828';}";
  html += "function setBadge(connected,statusName,status){var b=document.getElementById('wf-status-badge');if(!b)return;b.textContent=statusName;b.style.background=badgeBg(connected,status);}";
  html += "function yOf(v,lo,hi,h){return h-8-(v-lo)/(hi-lo)*(h-16);}";
  html += "function resizeCanvas(){var p=c.parentElement;var cssW=p?Math.max(280,p.clientWidth-2):320;var cssH=chartH;var dpr=window.devicePixelRatio||1;c.width=Math.round(cssW*dpr);c.height=Math.round(cssH*dpr);c.style.width=cssW+'px';c.style.height=cssH+'px';x.setTransform(1,0,0,1,0,0);x.scale(dpr,dpr);draw();}";
  html += "function draw(){var cc=window.portalChartColors?window.portalChartColors():{bg:'#fff',grid:'#ccc',fg:'#333'};var w=parseFloat(c.style.width)||320,h=parseFloat(c.style.height)||chartH;x.fillStyle=cc.bg;x.fillRect(0,0,w,h);";
  html += "x.strokeStyle=cc.grid;x.strokeRect(0.5,0.5,w-1,h-1);x.fillStyle=cc.fg;x.font='12px sans-serif';";
  html += "if(d.length<1){x.fillText('Collecting samples…',10,h/2);return;}";
  html += "var lo=-100,hi=-30,i,m;";
  html += "for(i=0;i<d.length;i++){m=d[i];if(m<lo)lo=m;if(m>hi)hi=m;}";
  html += "if(warnRssi<lo)lo=warnRssi-2;if(warnRssi>hi)hi=warnRssi+2;if(badRssi<lo)lo=badRssi-2;if(badRssi>hi)hi=badRssi+2;";
  html += "if(hi-lo<8){lo-=4;hi+=4;}";
  html += "x.fillText(lo+' dBm',4,h-4);x.fillText(hi+' dBm',4,14);";
  html += "x.strokeStyle='rgba(239,108,0,0.65)';x.setLineDash([4,4]);x.beginPath();x.moveTo(10,yOf(warnRssi,lo,hi,h));x.lineTo(w-10,yOf(warnRssi,lo,hi,h));x.stroke();";
  html += "x.strokeStyle='rgba(198,40,40,0.75)';x.beginPath();x.moveTo(10,yOf(badRssi,lo,hi,h));x.lineTo(w-10,yOf(badRssi,lo,hi,h));x.stroke();x.setLineDash([]);";
  html += "x.strokeStyle='#04AA6D';x.lineWidth=1.5;x.beginPath();";
  html += "for(i=0;i<d.length;i++){var slot=maxPts-d.length+i;var px=10+slot*(w-20)/Math.max(1,maxPts-1);";
  html += "var py=yOf(d[i],lo,hi,h);if(i===0)x.moveTo(px,py);else x.lineTo(px,py);}";
  html += "x.stroke();}";
  html += "function poll(){fetch('/api/wifi').then(function(r){return r.json();}).then(function(j){";
  html += "setBadge(j.connected,j.statusName,j.status);set('wf-st',String(j.status));";
  html += "set('wf-ssid',j.connected?j.ssid:'—');set('wf-host',j.hostname||'—');";
  html += "set('wf-ip',j.connected?j.ip:'—');set('wf-gw',j.connected?j.gateway:'—');";
  html += "set('wf-sn',j.connected?j.subnet:'—');set('wf-dns',j.connected?j.dns:'—');";
  html += "set('wf-ch',j.connected?String(j.channel):'—');set('wf-mac',j.mac||'—');";
  html += "set('wf-bssid',j.connected&&j.bssid?j.bssid:'—');";
  html += "set('wf-bssid-lock',j.bssidLock?j.bssidLock:'— (strongest AP)');";
  html += "if(j.connected&&typeof j.rssi==='number'){var q=qualityOf(j.rssi);set('wf-rssi',j.rssi+' dBm');set('wf-quality',q);set('wf-signal-now',j.rssi+' dBm '+q);";
  html += "var rc=document.getElementById('wf-rssi'),qc=document.getElementById('wf-quality'),sc=document.getElementById('wf-signal-now');var col=colorOf(j.rssi);if(rc)rc.style.color=col;if(qc)qc.style.color=col;if(sc)sc.style.color=col;";
  html += "d.push(j.rssi);if(d.length>maxPts)d.shift();var sum=0;for(var k=0;k<d.length;k++)sum+=d[k];set('wf-avg',(sum/d.length).toFixed(1)+' dBm');draw();}";
  html += "else{set('wf-rssi','—');set('wf-quality','');set('wf-signal-now','—');set('wf-avg','—');var rc=document.getElementById('wf-rssi'),sc=document.getElementById('wf-signal-now');if(rc)rc.style.color='';if(sc)sc.style.color='';d=[];draw();}}).catch(function(){});}";
  html += "window.addEventListener('resize',resizeCanvas);window.addEventListener('orientationchange',resizeCanvas);window.addEventListener('portal-theme-change',resizeCanvas);resizeCanvas();poll();setInterval(poll,pollMs);})();";
  html += "</script>";

  html += "</section>";
}

AsyncWebServer server(80);
static AsyncWebSocket wsLog("/api/logs/ws");
static uint32_t wsLogBroadcastSeq = 0;
bool serverSetup = false;

static void spaWebRegisterGet(const char *path, SpaWebHandlerFn handler)
{
  server.on(path, HTTP_GET, [handler](AsyncWebServerRequest *request) {
    spaWebDispatch(handler, request);
  });
}

static void spaWebRegisterPost(const char *path, SpaWebHandlerFn handler)
{
  server.on(path, HTTP_POST, [handler](AsyncWebServerRequest *request) {
    spaWebDispatch(handler, request);
  });
}

static void spaWebRegisterPostWithBody(const char *path, SpaWebHandlerFn handler)
{
  server.on(path, HTTP_POST, [handler](AsyncWebServerRequest *request) {
    spaWebDispatch(handler, request);
  }, NULL, handleBody);
}

static void onWsLogEvent(AsyncWebSocket *wsServer, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len)
{
  (void)arg;
  (void)data;
  (void)len;
  if (type == WS_EVT_CONNECT)
  {
    spaWebTouchHttpActivity();
    String hist;
    webLogBufferBuildJsonFull(hist);
    client->text(hist);
    wsLogBroadcastSeq = webLogBufferNewestSeq();
  }
  else if (type == WS_EVT_DISCONNECT && wsServer->count() == 0)
  {
    wsLogBroadcastSeq = webLogBufferNewestSeq();
  }
}

static void spaWebServerLogPoll()
{
  wsLog.cleanupClients();
  if (wsLog.count() == 0)
  {
    return;
  }
  const uint32_t newest = webLogBufferNewestSeq();
  if (newest <= wsLogBroadcastSeq)
  {
    return;
  }
  String delta;
  webLogBufferAppendJsonDelta(wsLogBroadcastSeq, newest, delta);
  if (delta.length() > 0)
  {
    spaWebTouchHttpActivity();
    wsLog.textAll(delta);
  }
  wsLogBroadcastSeq = newest;
}

#include "lwip/tcpip.h"

/**
 * lwIP heartbeat watchdog. The device can freeze with the tcpip task blocked forever (no ping,
 * no HTTP, ARP-level only) while loop() keeps running — reproduced under sustained portal
 * transfers on every AsyncTCP/ESPAsyncWebServer version tried. tcpip_try_callback() posts a
 * no-op into the tcpip task WITHOUT blocking (unlike every socket API); if the echo doesn't
 * come back, the stack is provably wedged and only a restart recovers it. Detection worst case
 * ~25s. Raw Serial for the fatal path so runtime log levels cannot mute it.
 */
static volatile unsigned long s_lwipEchoMs = 0;
static unsigned long s_lwipBeatSentMs = 0;
static uint8_t s_lwipBeatFailStreak = 0;

static void spaWebLwipEcho(void *)
{
  s_lwipEchoMs = millis();
}

static void spaWebLwipHeartbeatTick()
{
  if (!serverSetup)
  {
    return;
  }
  const unsigned long nowMs = millis();
  if (nowMs - s_lwipBeatSentMs < 10000UL)
  {
    return;
  }
  // Evaluate the previous beat before sending the next one.
  if (s_lwipBeatSentMs != 0 && s_lwipEchoMs < s_lwipBeatSentMs)
  {
    s_lwipBeatFailStreak++;
    Serial.printf("[NetProbe] lwIP heartbeat missed (%u/2)\r\n",
                  static_cast<unsigned>(s_lwipBeatFailStreak));
  }
  else
  {
    s_lwipBeatFailStreak = 0;
  }
  if (s_lwipBeatFailStreak >= 2)
  {
#if defined(DIAG_FAULT_CAPTURE)
    faultCaptureAppendf("[fault] lwIP heartbeat watchdog freeHeap=%u",
                        static_cast<unsigned>(ESP.getFreeHeap()));
#endif
    setLastRestartReason("lwIP heartbeat watchdog");
    Serial.println("[NetProbe] lwIP heartbeat dead twice, restarting");
    delay(50);
    ESP.restart();
  }
  s_lwipBeatSentMs = nowMs;
  if (tcpip_try_callback(&spaWebLwipEcho, nullptr) != ERR_OK)
  {
    // Full tcpip mailbox counts as a missed beat: the echo can never be queued.
    s_lwipEchoMs = 0;
  }
}

#include "ping/ping_sock.h"

/**
 * Wi-Fi path watchdog. Bench-reproduced failure mode on this Arduino 2.x core: under sustained
 * large HTTP transfers the Wi-Fi driver's RX path dies silently — no ping/HTTP from outside,
 * while WiFi.status() stays WL_CONNECTED, every task looks Blocked-normal, heap is healthy and
 * lwIP still answers tcpip_try_callback echoes (so loopback probes CANNOT see it; only traffic
 * across the radio can). Async esp_ping to the gateway exercises the real TX+RX radio path
 * without ever blocking loop(). Armed only after the first success on a connection so a router
 * that drops ICMP can never cause a restart loop. Three consecutive misses while associated
 * (~30s) → restart. Raw Serial so runtime log levels cannot mute the fatal path.
 */
static volatile uint8_t s_gwPingOutcome = 0; // 0 pending/none, 1 success, 2 timeout
static uint8_t s_gwPingFailStreak = 0;
static bool s_gwPingArmed = false;
static unsigned long s_gwPingLastMs = 0;

static void spaWebGwPingSuccess(esp_ping_handle_t, void *)
{
  s_gwPingOutcome = 1;
}

static void spaWebGwPingTimeout(esp_ping_handle_t, void *)
{
  s_gwPingOutcome = 2;
}

static void spaWebGwPingEnd(esp_ping_handle_t h, void *)
{
  esp_ping_delete_session(h);
}

static void spaWebWifiPathWatchdogTick()
{
  if (!serverSetup || WiFi.status() != WL_CONNECTED)
  {
    s_gwPingArmed = false;
    s_gwPingFailStreak = 0;
    s_gwPingOutcome = 0;
    return;
  }
  const unsigned long nowMs = millis();
  if (nowMs - s_gwPingLastMs < 10000UL)
  {
    return;
  }
  s_gwPingLastMs = nowMs;

  // Evaluate the previous probe before launching the next one.
  if (s_gwPingOutcome == 1)
  {
    s_gwPingArmed = true;
    s_gwPingFailStreak = 0;
  }
  else if (s_gwPingOutcome == 2 && s_gwPingArmed)
  {
    s_gwPingFailStreak++;
    Serial.printf("[NetProbe] gateway ping missed (%u/3)\r\n",
                  static_cast<unsigned>(s_gwPingFailStreak));
    if (s_gwPingFailStreak >= 3)
    {
#if defined(DIAG_FAULT_CAPTURE)
      faultCaptureAppendf("[fault] Wi-Fi path watchdog freeHeap=%u",
                          static_cast<unsigned>(ESP.getFreeHeap()));
#endif
      setLastRestartReason("Wi-Fi path watchdog");
      Serial.println("[NetProbe] Wi-Fi RX path dead (3 gateway pings lost while associated), restarting");
      delay(50);
      ESP.restart();
    }
  }
  s_gwPingOutcome = 0;

  const IPAddress gw = WiFi.gatewayIP();
  if (gw == IPAddress(0, 0, 0, 0))
  {
    return;
  }
  esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
  cfg.target_addr.type = IPADDR_TYPE_V4;
  cfg.target_addr.u_addr.ip4.addr = static_cast<uint32_t>(gw);
  cfg.count = 1;
  cfg.timeout_ms = 3000;
  esp_ping_callbacks_t cbs = {};
  cbs.on_ping_success = spaWebGwPingSuccess;
  cbs.on_ping_timeout = spaWebGwPingTimeout;
  cbs.on_ping_end = spaWebGwPingEnd;
  esp_ping_handle_t handle;
  if (esp_ping_new_session(&cfg, &cbs, &handle) == ESP_OK)
  {
    esp_ping_start(handle);
  }
}

/**
 * Every 15s from loop(): print scheduler state of the network tasks so a wedged lwIP stack can
 * be post-mortemed over serial while loop() is still alive (the freeze presents as: no ping, no
 * HTTP, RS485 logs continue). States: 0=Running 1=Ready 2=Blocked 3=Suspended 4=Deleted
 * -1=task not found. Raw Serial so runtime log-level changes cannot mute it.
 */
static void spaWebNetTaskProbe()
{
#ifndef SPA_NET_TASK_PROBE
  return; // diagnostic serial chatter; enable with -DSPA_NET_TASK_PROBE when investigating
#else
  static unsigned long lastMs = 0;
  const unsigned long nowMs = millis();
  if (nowMs - lastMs < 15000UL)
  {
    return;
  }
  lastMs = nowMs;
  static const char *const kNames[] = {"async_tcp", "tiT", "wifi", "arduino_events"};
  char line[144];
  size_t off = 0;
  for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); i++)
  {
    TaskHandle_t h = xTaskGetHandle(kNames[i]);
    const int st = h ? static_cast<int>(eTaskGetState(h)) : -1;
    off += snprintf(line + off, sizeof(line) - off, "%s=%d ", kNames[i], st);
  }
  Serial.printf("[NetProbe] %swifi=%d heap=%u\r\n", line, static_cast<int>(WiFi.status()),
                static_cast<unsigned>(ESP.getFreeHeap()));
#endif
}

static void spaWebHttpLivenessTick()
{
#if HTTP_LIVENESS_WATCHDOG
  if (!serverSetup || WiFi.status() != WL_CONNECTED)
  {
    return;
  }
  // A large portal transfer can legitimately monopolize sockets/heap for its whole lifetime;
  // probing (and worse, restarting) during one turns a slow page into a reboot. Defer instead.
  if (s_portalLargePageInFlight != 0)
  {
    return;
  }
  const unsigned long nowMs = millis();
  if (nowMs < HTTP_LIVENESS_MIN_UPTIME_MS)
  {
    return;
  }
  if (nowMs - s_lastHttpProbeMs < HTTP_LIVENESS_PROBE_INTERVAL_MS)
  {
    return;
  }
  s_lastHttpProbeMs = nowMs;

  if (spaWebHttpLivenessProbeOnce())
  {
    s_httpProbeFailStreak = 0;
    return;
  }
  if (s_httpProbeFailStreak < 255)
  {
    s_httpProbeFailStreak++;
  }
  Log.warning(F("[Web]: HTTP liveness probe failed (%u/%u)" CR),
              static_cast<unsigned>(s_httpProbeFailStreak),
              static_cast<unsigned>(HTTP_LIVENESS_PROBE_FAIL_MAX));
  if (s_httpProbeFailStreak < HTTP_LIVENESS_PROBE_FAIL_MAX)
  {
    return;
  }
  static bool restartArmed = true;
  if (!restartArmed)
  {
    return;
  }
  restartArmed = false;
#if defined(DIAG_FAULT_CAPTURE)
  faultCaptureAppendf("[fault] HTTP liveness probe fail streak=%u freeHeap=%u maxAlloc=%u",
                      static_cast<unsigned>(s_httpProbeFailStreak),
                      static_cast<unsigned>(ESP.getFreeHeap()),
                      static_cast<unsigned>(ESP.getMaxAllocHeap()));
#endif
  setLastRestartReason("HTTP liveness watchdog");
  Log.error(F("[Web]: HTTP liveness probe failed %u times, restarting" CR),
            static_cast<unsigned>(HTTP_LIVENESS_PROBE_FAIL_MAX));
  delay(50);
  ESP.restart();
#endif
}

void spaWebServerSetup()
{
  if (!LittleFS.begin(FORMAT_LITTLEFS_IF_FAILED))
  {
    Log.error("[Web]: Error LittleFS Mount Failed");
  }
  else
  {
    Log.notice("[Web]: LittleFS Mounted" CR);
    listDir(LittleFS, "/", 3);
  }
  Log.notice("[Web]: Web App config" CR);
  File envFile = LittleFS.open("/.env", "r");
  if (envFile)
  {
    // Log.notice("[Web]: .env file found" CR);
    while (envFile.available())
    {
      String line = envFile.readStringUntil('\n');
      Log.notice("[Web]: /.env - %s" CR, line.c_str());
    }
    envFile.close();
  }
  else
  {
    Log.error("[Web]: .env file not found" CR);
  }
  // put your setup code here, to run once:
  Log.verbose(F("[Web]: spaWebServerSetup()" CR));
}

void spaWebServerLoop()
{
  if (!serverSetup)
  {
    spaWebRegisterGet("/", handleStatus);
    spaWebRegisterGet("/state", handleState);
    spaWebRegisterGet("/config", handleConfig);
    spaWebRegisterGet("/status", handleStatus);
    spaWebRegisterGet("/api/version", handleVersion);
    spaWebRegisterGet("/api/diagnostics", handleDiagnostics);
    spaWebRegisterGet("/api/wifi", handleWifi);
    spaWebRegisterGet("/api/mqtt", handleMqtt);
    spaWebRegisterGet("/api/status/controls", handleStatusControlsApi);
    spaWebRegisterGet("/api/status/summary", handleStatusSummaryApi);
    spaWebRegisterGet("/api/status/histories", handleStatusHistoriesApi);
    spaWebRegisterGet("/api/state/littlefs", handleStateLittleFsApi);
    spaWebRegisterGet("/api/diag/toggle", handleDiagToggleApi);
    spaWebRegisterGet("/api/diag/toggle_sequence", handleDiagToggleSequenceApi);
    spaWebRegisterGet("/api/diag/light1_next_cts", handleDiagLight1NextCtsApi);
    spaWebRegisterGet("/api/diag/light1_next_cts_window", handleDiagLight1NextCtsWindowApi);
    spaWebRegisterGet("/api/rs485/raw", handleRs485Raw);
    spaWebRegisterGet("/api/rs485/history", handleRs485History);
    spaWebRegisterPost("/api/rs485/retry", handleRs485Retry);
    spaWebRegisterGet("/api/rs485", handleRs485);
    spaWebRegisterGet("/api/logs", handleLogsApi);
    spaWebRegisterGet("/api/logs/config", handleLogsConfigGet);
    spaWebRegisterPostWithBody("/api/logs/config", handleLogsConfigPost);
    spaWebRegisterGet("/api/config/filter", handleConfigFilterGet);
    spaWebRegisterPostWithBody("/api/config/filter", handleConfigFilterPost);
    spaWebRegisterGet("/api/config/preferences", handleConfigPreferencesGet);
    spaWebRegisterPostWithBody("/api/config/preferences", handleConfigPreferencesPost);
    spaWebRegisterGet("/api/config/fault-log/history", handleConfigFaultLogHistoryGet);
    spaWebRegisterPostWithBody("/api/config/fault-log/history", handleConfigFaultLogHistoryPost);
    spaWebRegisterGet("/api/config/fault-log", handleConfigFaultLogGet);
    spaWebRegisterGet("/api/config/export", handleConfigExport);
    spaWebRegisterPostWithBody("/api/config/import", handleConfigImportPost);
    spaWebRegisterGet("/logs", handleLogsPage);
    wsLog.onEvent(onWsLogEvent);
    server.addHandler(&wsLog);
#ifdef spaEpaper
    spaWebRegisterGet("/panel.jpg", handleepdpanel);
#endif
    server.on("/restart", HTTP_GET, [](AsyncWebServerRequest *request)
              {
      spaWebTouchHttpActivity();
      Log.notice(F("[Web]: Restart requested by %p" CR), request->client()->remoteIP());
      setLastRestartReason("Web restart");
      AsyncWebServerResponse *response = request->beginResponse(302);
      response->addHeader("Location", "/");
      request->send(response);
      delay(1000);
      ESP.restart(); });

    // Balboa cloud emulation

    server.on("/devices/sci", HTTP_OPTIONS, [](AsyncWebServerRequest *request) {
      spaWebTouchHttpActivity();
      handleOptionsData(request);
    });
    spaWebRegisterPostWithBody("/devices/sci", handleData);

    server.on("/users/login", HTTP_OPTIONS, [](AsyncWebServerRequest *request) {
      spaWebTouchHttpActivity();
      handleOptionsLoginData(request);
    });
    spaWebRegisterPostWithBody("/users/login", handleLoginData);

    server.onNotFound([](AsyncWebServerRequest *request) {
      spaWebTouchHttpActivity();
      handleNotFound(request);
    });

    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Requested-With");

    server.begin();
    serverSetup = true;
    Log.notice(F("[Web]: Web server started at http://%p/" CR), WiFi.localIP());
  }
  spaWebNetTaskProbe();
  spaWebLwipHeartbeatTick();
  spaWebWifiPathWatchdogTick();
  spaWebHttpLivenessTick();
  spaWebServerLogPoll();
}

#ifdef spaEpaper
void handleepdpanel(AsyncWebServerRequest *request)
{
  Log.verbose("[Web]: Request %s received from %p - size %d" CR, request->url().c_str(), request->client()->remoteIP(), jpegSize);
  if (captureToJPEG() > 0)
  {
    // Send the BMP image as a response
    AsyncWebServerResponse *response = request->beginResponse_P(200, "image/jpeg", jpegBuffer, jpegSize);
    response->addHeader("Content-Disposition", "inline; filename=\"framebuffer.jpeg\"");
    request->send(response);
  }
  else
  {
    request->send(404, "text/plain", "Image not available");
  }
}
#endif

#define portalBaseStyle "<style>:root{--bg:#f4f7f8;--panel:#fff;--text:#1f2933;--muted:#5f6c7b;--brand:#037e52;--brandActive:#4b5563;--border:#d4dbe1;--focus:#0f4a87;--heading:#0f4a87;--surface-2:#fafbfc;--chart-bg:#fff;--chart-grid:#ccc;--chart-fg:#333;--chart-grid-line:#eef1f4;--space-1:6px;--space-2:10px;--space-3:14px;--space-4:20px;color-scheme:light;}@media (prefers-color-scheme:dark){:root:not([data-theme=light]){--bg:#141a1f;--panel:#1e262d;--text:#e8edf2;--muted:#9aa8b5;--brand:#04a86d;--brandActive:#6b7280;--border:#3a4550;--focus:#6eb3ff;--heading:#8ec5ff;--surface-2:#252e36;--chart-bg:#1e262d;--chart-grid:#3a4550;--chart-fg:#c8d4de;--chart-grid-line:#2a343c;color-scheme:dark;}}:root[data-theme=dark]{--bg:#141a1f;--panel:#1e262d;--text:#e8edf2;--muted:#9aa8b5;--brand:#04a86d;--brandActive:#6b7280;--border:#3a4550;--focus:#6eb3ff;--heading:#8ec5ff;--surface-2:#252e36;--chart-bg:#1e262d;--chart-grid:#3a4550;--chart-fg:#c8d4de;--chart-grid-line:#2a343c;color-scheme:dark;}:root[data-theme=light]{color-scheme:light;}*{box-sizing:border-box;}body{margin:0;font-family:Arial,Helvetica,sans-serif;background:var(--bg);color:var(--text);line-height:1.5;}html,body{max-width:100%;overflow-x:hidden;}img,canvas{display:block;max-width:100%;height:auto;}.skip-link{position:absolute;left:10px;top:-48px;z-index:999;background:var(--focus);color:#fff;padding:10px 12px;border-radius:6px;text-decoration:none;}.skip-link:focus{top:10px;outline:3px solid #fff;outline-offset:2px;}.page{max-width:980px;margin:0 auto;padding:var(--space-3);}h1{color:var(--heading);font-size:1.05rem;margin:0 0 var(--space-2) 0;line-height:1.3;}.panel{background:var(--panel);border:1px solid var(--border);border-radius:10px;padding:var(--space-3);margin-bottom:var(--space-3);box-shadow:0 1px 2px rgba(0,0,0,.04);}ul{list-style:none;margin:0;padding:0;}li{padding:var(--space-1) 0;border-bottom:1px dashed var(--border);overflow-wrap:anywhere;word-break:break-word;}li:last-child{border-bottom:none;}.spacer{height:8px;border-bottom:none;padding:0;}.portal-nav-bar{display:flex;align-items:stretch;gap:var(--space-1);margin-bottom:var(--space-3);}.top-nav{display:flex;flex-wrap:wrap;gap:var(--space-1);flex:1 1 auto;margin-bottom:0;min-width:0;}.portal-nav-util{flex:0 0 auto;display:flex;align-items:stretch;}.top-nav a{border:none;color:#fff;padding:12px 16px;text-align:center;text-decoration:none;display:inline-flex;justify-content:center;align-items:center;font-size:15px;line-height:1.2;min-height:44px;cursor:pointer;background-color:var(--brand);border-radius:8px;flex:1 1 170px;font-weight:600;transition:background-color .15s ease,transform .15s ease}.top-nav a.active{background-color:var(--brandActive);color:#fff}@media (hover:hover){.top-nav a:hover{background-color:var(--brandActive)}}.top-nav a:focus-visible{outline:3px solid var(--focus);outline-offset:2px}.top-nav a:active{transform:translateY(1px)}@media (prefers-reduced-motion:reduce){.top-nav a{transition:none}}.portal-theme-toggle{display:inline-flex!important;align-items:center;justify-content:center;gap:6px;flex:0 0 auto!important;width:auto!important;min-width:88px!important;padding:12px 14px!important;background:var(--panel)!important;color:var(--text)!important;border:1px solid var(--border)!important;font-size:14px!important;font-weight:600!important;cursor:pointer;border-radius:8px;min-height:44px;transition:background-color .15s ease,transform .15s ease;box-shadow:none!important}.portal-theme-toggle__icon{display:inline-flex;line-height:0;flex-shrink:0}.portal-theme-toggle__icon svg{display:block}@media (hover:hover){.portal-theme-toggle:hover{background:var(--surface-2)!important;color:var(--text)!important;transform:none!important}}.portal-theme-toggle:focus-visible{outline:3px solid var(--focus);outline-offset:2px}.portal-theme-toggle:active{transform:translateY(1px)}@media (min-width:641px){.portal-nav-util .portal-theme-toggle{min-width:44px!important;width:44px!important;padding:10px!important;border-radius:999px}.portal-nav-util .portal-theme-toggle__label{position:absolute;width:1px;height:1px;padding:0;margin:-1px;overflow:hidden;clip:rect(0,0,0,0);white-space:nowrap;border:0}}.top-nav-mobile{display:none}.top-nav-mobile__summary{display:block;cursor:pointer;list-style:none;padding:0;margin:0}.top-nav-mobile__summary::-webkit-details-marker{display:none}.top-nav-mobile__summary::marker{content:''}.top-nav-mobile__summary-inner{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:10px 14px;background:var(--panel);border:1px solid var(--border);border-radius:8px;font-weight:600;font-size:15px;line-height:1.2;box-shadow:0 1px 3px rgba(0,0,0,.06)}.top-nav-mobile__context{color:var(--text);flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.top-nav-mobile__menu{display:flex;align-items:center;gap:6px;color:var(--brand);font-weight:700;flex-shrink:0}.top-nav-mobile__chev{border:solid currentColor;border-width:0 2px 2px 0;display:inline-block;padding:3px;transform:rotate(45deg);transition:transform .15s ease;margin-top:-2px}.top-nav-mobile[open] .top-nav-mobile__chev{transform:rotate(225deg);margin-top:2px}.top-nav-mobile__panel{display:flex;flex-direction:column;gap:var(--space-1);margin-top:var(--space-2);padding:var(--space-2);background:var(--panel);border:1px solid var(--border);border-radius:8px}.top-nav-mobile__panel a{border:none;color:#fff;padding:12px 16px;text-align:center;text-decoration:none;display:inline-flex;justify-content:center;align-items:center;font-size:15px;line-height:1.2;min-height:44px;cursor:pointer;background-color:var(--brand);border-radius:8px;font-weight:600;width:100%;box-sizing:border-box;transition:background-color .15s ease,transform .15s ease}.top-nav-mobile__panel a.active{background-color:var(--brandActive);color:#fff}@media (hover:hover){.top-nav-mobile__panel a:hover{background-color:var(--brandActive)}}.top-nav-mobile__panel a:focus-visible{outline:3px solid var(--focus);outline-offset:2px}.top-nav-mobile__panel a:active{transform:translateY(1px)}.top-nav-mobile__panel .portal-theme-toggle{width:100%!important;box-sizing:border-box}.portal-nav-scroll-sentinel{height:1px;width:100%;margin:0;padding:0;border:0;pointer-events:none;opacity:0;position:relative}@media (prefers-reduced-motion:reduce){.top-nav-mobile__chev{transition:none}.top-nav-mobile__panel a{transition:none}.portal-theme-toggle{transition:none}}button{border:none;color:#fff;padding:12px 16px;text-align:center;text-decoration:none;display:inline-flex;justify-content:center;align-items:center;font-size:15px;line-height:1.2;min-height:44px;cursor:pointer;background-color:var(--brand);border-radius:8px;flex:1 1 170px;font-weight:600;transition:background-color .15s ease,transform .15s ease;}.active{background-color:var(--brandActive);color:#fff;}@media (hover:hover){button:hover{background-color:var(--brandActive);}}button:focus-visible{outline:3px solid var(--focus);outline-offset:2px;}button:active{transform:translateY(1px);}.panel-image{width:100%;max-width:600px;margin:0 auto var(--space-3) auto;border-radius:8px;}.chart-title{margin:12px 0 6px 0;color:var(--muted);}.chart-wrap{width:100%;max-width:100%;overflow:hidden;border:1px solid var(--chart-grid);background:var(--chart-bg);border-radius:6px;}#wf-rssi,#wf-quality{font-weight:700;}@media (max-width:640px){.portal-nav-bar{display:none !important}.top-nav-mobile{display:block;margin-bottom:var(--space-3)}.top-nav-mobile__summary{position:sticky;top:0;z-index:50}.top-nav-mobile__summary .top-nav-mobile__summary-inner{background:var(--panel)}body.portal-nav-compact .top-nav-mobile__summary-inner{padding:6px 10px;font-size:0.88rem}body.portal-nav-compact .top-nav-mobile__menu-text{display:none}.page{padding:var(--space-2);}button{flex:1 1 100%;width:100%;}.log-controls button,.range-toggle button,.status-temp-units-toggle button,.equip-btn,.portal-theme-toggle{width:auto!important;flex:0 1 auto!important;min-width:0}.top-nav-mobile__panel .portal-theme-toggle{width:100%!important}.panel{padding:var(--space-2);}h1{font-size:1rem;}}@media (prefers-reduced-motion:reduce){button{transition:none;}}</style>"

#define portalHeadIcon "<link rel='icon' href='/assets/style/hottubbing.webp' type='image/x-icon' />"

#define portalThemeScript "<script>(function(){var KEY='portal-theme';var MODES=['system','light','dark'];var LABELS={system:'Auto',light:'Light',dark:'Dark'};var ICONS={system:'<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"18\" height=\"18\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" aria-hidden=\"true\"><circle cx=\"12\" cy=\"12\" r=\"10\"/><path d=\"M12 2a10 10 0 0 0 0 20V2z\" fill=\"currentColor\" stroke=\"none\"/></svg>',light:'<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"18\" height=\"18\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\" aria-hidden=\"true\"><circle cx=\"12\" cy=\"12\" r=\"4\"/><path d=\"M12 2v2M12 20v2M4.93 4.93l1.41 1.41M17.66 17.66l1.41 1.41M2 12h2M20 12h2M4.93 19.07l1.41-1.41M17.66 6.34l1.41-1.41\"/></svg>',dark:'<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"18\" height=\"18\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\" aria-hidden=\"true\"><path d=\"M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z\"/></svg>'};function getStored(){try{var v=localStorage.getItem(KEY);if(MODES.indexOf(v)>=0)return v;}catch(e){}return'system';}function syncToggles(mode){var lbl=LABELS[mode]||'Auto';var icon=ICONS[mode]||ICONS.system;document.querySelectorAll('[data-portal-theme-toggle]').forEach(function(b){var ic=b.querySelector('.portal-theme-toggle__icon');var lb=b.querySelector('.portal-theme-toggle__label');if(ic)ic.innerHTML=icon;if(lb)lb.textContent=lbl;b.setAttribute('aria-label','Theme: '+lbl+'. Click to change.');b.setAttribute('title','Theme: '+lbl+' (click to change)');});}function apply(mode){var r=document.documentElement;if(mode==='light'||mode==='dark')r.setAttribute('data-theme',mode);else r.removeAttribute('data-theme');syncToggles(mode);window.dispatchEvent(new Event('portal-theme-change'));}function setTheme(mode){if(MODES.indexOf(mode)<0)mode='system';try{localStorage.setItem(KEY,mode);}catch(e){}apply(mode);}window.portalCycleTheme=function(){var i=MODES.indexOf(getStored());setTheme(MODES[(i+1)%MODES.length]);};window.portalGetTheme=getStored;window.portalChartColors=function(){var s=getComputedStyle(document.documentElement);function g(n,d){var v=s.getPropertyValue(n);return(v&&v.trim())?v.trim():d;}return{bg:g('--chart-bg','#fff'),grid:g('--chart-grid','#ccc'),fg:g('--chart-fg','#333'),gridLine:g('--chart-grid-line','#eef1f4')};};apply(getStored());window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change',function(){if(getStored()==='system')window.dispatchEvent(new Event('portal-theme-change'));});if(document.readyState==='loading')document.addEventListener('DOMContentLoaded',function(){syncToggles(getStored());});else syncToggles(getStored());})();</script>"

#define portalHeadNavScript "<script>(function(){var m=window.matchMedia('(max-width:640px)');var io=null;function setup(){document.body.classList.remove('portal-nav-compact');if(io){io.disconnect();io=null;}if(!m.matches)return;var s=document.querySelector('.portal-nav-scroll-sentinel');if(!s)return;io=new IntersectionObserver(function(e){e.forEach(function(x){document.body.classList.toggle('portal-nav-compact',!x.isIntersecting);});},{threshold:0});io.observe(s);}if(document.readyState==='loading')document.addEventListener('DOMContentLoaded',setup);else setup();m.addEventListener('change',setup);})();</script>"

#define portalThemeToggleBtn "<button type=\"button\" class=\"portal-theme-toggle\" data-portal-theme-toggle onclick=\"portalCycleTheme()\" aria-label=\"Theme: Auto. Click to change.\"><span class=\"portal-theme-toggle__icon\" aria-hidden=\"true\"><svg xmlns=\"http://www.w3.org/2000/svg\" width=\"18\" height=\"18\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" aria-hidden=\"true\"><circle cx=\"12\" cy=\"12\" r=\"10\"/><path d=\"M12 2a10 10 0 0 0 0 20V2z\" fill=\"currentColor\" stroke=\"none\"/></svg></span><span class=\"portal-theme-toggle__label\">Auto</span></button>"

#define portalSemanticDarkStyle "<style>@media (prefers-color-scheme:dark){:root:not([data-theme=light]) .heat-hero--ok{border-color:#4a7090;background:#1a2838;color:#c8dce8}:root:not([data-theme=light]) .heat-hero--ok .heat-hero-icon{color:#8ec5ff}:root:not([data-theme=light]) .heat-hero--init{border-color:#9a8530;background:#3a3218;color:#f5e6a8}:root:not([data-theme=light]) .heat-hero--init .heat-hero-icon{color:#e6c200}:root:not([data-theme=light]) .heat-hero--alert{border-color:#a05050;background:#3a2020;color:#ffcdd2}:root:not([data-theme=light]) .heat-hero--alert .heat-hero-icon{color:#ef9a9a}:root:not([data-theme=light]) .heat-hero--heat-idle{border-color:#4a5560;background:#2a3038;color:#b0bec5}:root:not([data-theme=light]) .heat-hero--heat-idle .heat-hero-icon{color:#9aa8b5}:root:not([data-theme=light]) .heat-hero--heat-on{border-color:#c06040;background:#3d2218;color:#ffccbc}:root:not([data-theme=light]) .heat-hero--heat-on .heat-hero-icon{color:#ffab91}:root:not([data-theme=light]) .heat-hero--heat-alt{border-color:#b89030;background:#3a3218;color:#fff8e1}:root:not([data-theme=light]) .heat-hero--heat-alt .heat-hero-icon{color:#ffe082}:root:not([data-theme=light]) .heat-hero--heat-reserved{border-color:#546e7a;background:#2a3238;color:#b0bec5}:root:not([data-theme=light]) .heat-hero--heat-reserved .heat-hero-icon{color:#90a4ae}:root:not([data-theme=light]) .status-reminder-banner--warn{border-color:#9a8530;background:#3a3218;color:#f5e6a8}:root:not([data-theme=light]) .status-reminder-banner--warn .status-reminder-banner-icon{color:#e6c200}:root:not([data-theme=light]) .status-reminder-banner--fault{border-color:#a05050;background:#3a2020;color:#ffcdd2}:root:not([data-theme=light]) .status-reminder-banner--fault .status-reminder-banner-icon{color:#ef9a9a}:root:not([data-theme=light]) .heat-chip{background:var(--surface-2);color:var(--text)}:root:not([data-theme=light]) .heat-chip--heat-idle{color:#b0bec5;background:#2a3038;border-color:#4a5560}:root:not([data-theme=light]) .heat-chip--heat-on{color:#ffab91;background:#3d2218;border-color:#8b4513}:root:not([data-theme=light]) .heat-chip--heat-alt{color:#ffe082;background:#3a3218;border-color:#8a7530}:root:not([data-theme=light]) .heat-chip--need-yes{color:#ef9a9a;background:#3d2020;border-color:#a05050}:root:not([data-theme=light]) .heat-chip--need-no{color:#90a4ae;background:#2a3238;border-color:#546e7a}:root:not([data-theme=light]) .equip-cell--off{background:#2a3038;border-color:#4a5560;color:var(--text)}:root:not([data-theme=light]) .equip-cell--low{background:#3a3218;border-color:#b89030;color:#f5e6a8}:root:not([data-theme=light]) .equip-cell--high,:root:not([data-theme=light]) .equip-cell--on{background:#1a3028;border-color:#4a9070;color:#a5d6a7}:root:not([data-theme=light]) .equip-cell.equip-absent{background:#252e36;border-color:#3a4550}:root:not([data-theme=light]) .equip-cell.equip-absent .equip-label{color:var(--muted)}:root:not([data-theme=light]) .equip-cell.equip-absent .equip-val{color:var(--text)}:root:not([data-theme=light]) .range-band-active-low{border-color:var(--focus);background:#1a2838;color:var(--text)}:root:not([data-theme=light]) .range-band-active-high{border-color:#ef5350;background:#3a2020;color:#ffcdd2}:root:not([data-theme=light]) .range-band-active-low .range-band-indicator,:root:not([data-theme=light]) .range-band-active-high .range-band-indicator{color:inherit}:root:not([data-theme=light]) .config-fault-sev--info{color:#a5d6a7;background:#1a3028}:root:not([data-theme=light]) .config-fault-sev--warning{color:#f5e6a8;background:#3a3218}:root:not([data-theme=light]) .config-fault-sev--alert{color:#ffcdd2;background:#3d2020}:root:not([data-theme=light]) .fw-pill-current{background:#1a2838;color:var(--heading);border-color:#4a7090}:root:not([data-theme=light]) .fw-pill-latest{background:var(--surface-2);color:var(--muted)}:root:not([data-theme=light]) .fw-pill-branch{background:#3a3218;color:#f5d090;border-color:#8a7530}:root:not([data-theme=light]) button.fw-danger-btn{color:#ffcdd2!important;border-color:#a05050!important;background:#3d2020!important}:root:not([data-theme=light]) .status-control-row input{background:var(--panel);color:var(--text)}:root:not([data-theme=light]) .config-filter-card input[type=time],:root:not([data-theme=light]) .config-filter-card input[type=number]{background:var(--panel);color:var(--text);border:1px solid var(--border)}:root:not([data-theme=light]) .kv-row--alert dt,:root:not([data-theme=light]) .kv-row--alert dd{color:#ef9a9a}:root:not([data-theme=light]) .kv-row--alert-warn dt,:root:not([data-theme=light]) .kv-row--alert-warn dd{color:#f5e6a8}}:root[data-theme=dark] .heat-hero--ok{border-color:#4a7090;background:#1a2838;color:#c8dce8}:root[data-theme=dark] .heat-hero--ok .heat-hero-icon{color:#8ec5ff}:root[data-theme=dark] .heat-hero--init{border-color:#9a8530;background:#3a3218;color:#f5e6a8}:root[data-theme=dark] .heat-hero--init .heat-hero-icon{color:#e6c200}:root[data-theme=dark] .heat-hero--alert{border-color:#a05050;background:#3a2020;color:#ffcdd2}:root[data-theme=dark] .heat-hero--alert .heat-hero-icon{color:#ef9a9a}:root[data-theme=dark] .heat-hero--heat-idle{border-color:#4a5560;background:#2a3038;color:#b0bec5}:root[data-theme=dark] .heat-hero--heat-idle .heat-hero-icon{color:#9aa8b5}:root[data-theme=dark] .heat-hero--heat-on{border-color:#c06040;background:#3d2218;color:#ffccbc}:root[data-theme=dark] .heat-hero--heat-on .heat-hero-icon{color:#ffab91}:root[data-theme=dark] .heat-hero--heat-alt{border-color:#b89030;background:#3a3218;color:#fff8e1}:root[data-theme=dark] .heat-hero--heat-alt .heat-hero-icon{color:#ffe082}:root[data-theme=dark] .heat-hero--heat-reserved{border-color:#546e7a;background:#2a3238;color:#b0bec5}:root[data-theme=dark] .heat-hero--heat-reserved .heat-hero-icon{color:#90a4ae}:root[data-theme=dark] .status-reminder-banner--warn{border-color:#9a8530;background:#3a3218;color:#f5e6a8}:root[data-theme=dark] .status-reminder-banner--warn .status-reminder-banner-icon{color:#e6c200}:root[data-theme=dark] .status-reminder-banner--fault{border-color:#a05050;background:#3a2020;color:#ffcdd2}:root[data-theme=dark] .status-reminder-banner--fault .status-reminder-banner-icon{color:#ef9a9a}:root[data-theme=dark] .heat-chip{background:var(--surface-2);color:var(--text)}:root[data-theme=dark] .heat-chip--heat-idle{color:#b0bec5;background:#2a3038;border-color:#4a5560}:root[data-theme=dark] .heat-chip--heat-on{color:#ffab91;background:#3d2218;border-color:#8b4513}:root[data-theme=dark] .heat-chip--heat-alt{color:#ffe082;background:#3a3218;border-color:#8a7530}:root[data-theme=dark] .heat-chip--need-yes{color:#ef9a9a;background:#3d2020;border-color:#a05050}:root[data-theme=dark] .heat-chip--need-no{color:#90a4ae;background:#2a3238;border-color:#546e7a}:root[data-theme=dark] .equip-cell--off{background:#2a3038;border-color:#4a5560;color:var(--text)}:root[data-theme=dark] .equip-cell--low{background:#3a3218;border-color:#b89030;color:#f5e6a8}:root[data-theme=dark] .equip-cell--high,:root[data-theme=dark] .equip-cell--on{background:#1a3028;border-color:#4a9070;color:#a5d6a7}:root[data-theme=dark] .equip-cell.equip-absent{background:#252e36;border-color:#3a4550}:root[data-theme=dark] .equip-cell.equip-absent .equip-label{color:var(--muted)}:root[data-theme=dark] .equip-cell.equip-absent .equip-val{color:var(--text)}:root[data-theme=dark] .range-band-active-low{border-color:var(--focus);background:#1a2838;color:var(--text)}:root[data-theme=dark] .range-band-active-high{border-color:#ef5350;background:#3a2020;color:#ffcdd2}:root[data-theme=dark] .range-band-active-low .range-band-indicator,:root[data-theme=dark] .range-band-active-high .range-band-indicator{color:inherit}:root[data-theme=dark] .config-fault-sev--info{color:#a5d6a7;background:#1a3028}:root[data-theme=dark] .config-fault-sev--warning{color:#f5e6a8;background:#3a3218}:root[data-theme=dark] .config-fault-sev--alert{color:#ffcdd2;background:#3d2020}:root[data-theme=dark] .fw-pill-current{background:#1a2838;color:var(--heading);border-color:#4a7090}:root[data-theme=dark] .fw-pill-latest{background:var(--surface-2);color:var(--muted)}:root[data-theme=dark] .fw-pill-branch{background:#3a3218;color:#f5d090;border-color:#8a7530}:root[data-theme=dark] button.fw-danger-btn{color:#ffcdd2!important;border-color:#a05050!important;background:#3d2020!important}:root[data-theme=dark] .status-control-row input{background:var(--panel);color:var(--text)}:root[data-theme=dark] .config-filter-card input[type=time],:root[data-theme=dark] .config-filter-card input[type=number]{background:var(--panel);color:var(--text);border:1px solid var(--border)}:root[data-theme=dark] .kv-row--alert dt,:root[data-theme=dark] .kv-row--alert dd{color:#ef9a9a}:root[data-theme=dark] .kv-row--alert-warn dt,:root[data-theme=dark] .kv-row--alert-warn dd{color:#f5e6a8}</style>"

#define portalLogsHeadExtraStyle "<style>.log-pre{min-height:260px;max-height:70vh;overflow:auto;background:#0f172a;color:#e2e8f0;font-family:ui-monospace,Menlo,Consolas,monospace;font-size:12px;line-height:1.45;padding:12px;border-radius:8px;white-space:pre-wrap;word-break:break-word;margin:0;border:1px solid var(--border)}.log-controls{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-bottom:12px}.log-controls input[type=text]{flex:1 1 140px;min-width:120px;padding:8px;border:1px solid var(--border);border-radius:6px;font-size:14px;background:var(--panel);color:var(--text)}.log-controls label{font-size:14px;color:var(--muted)}.log-controls select{padding:8px;border-radius:6px;border:1px solid var(--border);font-size:14px;background:var(--panel);color:var(--text)}</style>"

/** True when portal global CSS marker is present (String: buffer scan; chunks: append stayed healthy). */
static bool portalHtmlSawGlobalCss(const String &html)
{
  return html.indexOf(F(":root{--bg:#f4f7f8")) >= 0;
}

static bool portalHtmlSawGlobalCss(const PortalHtmlChunks &html)
{
  return html.healthy();
}

/** Append shared portal `<head>` with sequential writes (no chained `String` temporaries).
 *  Returns whether portal global CSS is present after the base-style append. */
template <typename HtmlOut>
static bool appendPortalHead(HtmlOut &html, const char *title, const char *viewportExtra = "",
                             const char *extraHeadStyle = nullptr)
{
  const size_t want = html.length() + 20000u;
  html.reserve(want);
  html += F("<head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1");
  if (viewportExtra != nullptr && viewportExtra[0] != '\0')
  {
    html += viewportExtra;
  }
  html += F("'><title>");
  html += title;
  html += F("</title>");
  html += portalHeadIcon;
  html += portalBaseStyle;
  const bool sawPortalCss = portalHtmlSawGlobalCss(html);
  html += portalSemanticDarkStyle;
  html += portalThemeScript;
  html += portalHeadNavScript;
  if (extraHeadStyle != nullptr && extraHeadStyle[0] != '\0')
  {
    html += extraHeadStyle;
  }
  html += F("</head>");
  return sawPortalCss;
}

static void logPortalHtmlMissingGlobalCss(bool sawPortalCss, size_t len, const char *pageTag)
{
  if (sawPortalCss)
  {
    return;
  }
  Log.error("[Web]: %s missing portal global CSS len=%u — sending without repair" CR, pageTag,
            static_cast<unsigned>(len));
}

#define webMenuStatus "<nav aria-label='Portal navigation' class='portal-nav'><div class='portal-nav-bar'><div class='top-nav'><a class='active' aria-current='page' href='/status'>Spa Status</a><a href='/config'>Spa Config</a><a href='/state'>ESP State</a><a href='/logs'>Logs</a><a href='/index.html'>Spa Website</a></div><div class='portal-nav-util'>" portalThemeToggleBtn "</div></div><details class='top-nav-mobile'><summary class='top-nav-mobile__summary'><span class='top-nav-mobile__summary-inner'><span class='top-nav-mobile__context'>Spa Status</span><span class='top-nav-mobile__menu'><span class='top-nav-mobile__chev' aria-hidden='true'></span><span class='top-nav-mobile__menu-text'>Menu</span></span></span></summary><div class='top-nav-mobile__panel' role='group' aria-label='Portal pages'><a class='active' aria-current='page' href='/status'>Spa Status</a><a href='/config'>Spa Config</a><a href='/state'>ESP State</a><a href='/logs'>Logs</a><a href='/index.html'>Spa Website</a>" portalThemeToggleBtn "</div></details></nav><div class='portal-nav-scroll-sentinel' aria-hidden='true'></div>"

#define webMenuConfig "<nav aria-label='Portal navigation' class='portal-nav'><div class='portal-nav-bar'><div class='top-nav'><a href='/status'>Spa Status</a><a class='active' aria-current='page' href='/config'>Spa Config</a><a href='/state'>ESP State</a><a href='/logs'>Logs</a><a href='/index.html'>Spa Website</a></div><div class='portal-nav-util'>" portalThemeToggleBtn "</div></div><details class='top-nav-mobile'><summary class='top-nav-mobile__summary'><span class='top-nav-mobile__summary-inner'><span class='top-nav-mobile__context'>Spa Config</span><span class='top-nav-mobile__menu'><span class='top-nav-mobile__chev' aria-hidden='true'></span><span class='top-nav-mobile__menu-text'>Menu</span></span></span></summary><div class='top-nav-mobile__panel' role='group' aria-label='Portal pages'><a href='/status'>Spa Status</a><a class='active' aria-current='page' href='/config'>Spa Config</a><a href='/state'>ESP State</a><a href='/logs'>Logs</a><a href='/index.html'>Spa Website</a>" portalThemeToggleBtn "</div></details></nav><div class='portal-nav-scroll-sentinel' aria-hidden='true'></div>"

#define webMenuState "<nav aria-label='Portal navigation' class='portal-nav'><div class='portal-nav-bar'><div class='top-nav'><a href='/status'>Spa Status</a><a href='/config'>Spa Config</a><a class='active' aria-current='page' href='/state'>ESP State</a><a href='/logs'>Logs</a><a href='/index.html'>Spa Website</a></div><div class='portal-nav-util'>" portalThemeToggleBtn "</div></div><details class='top-nav-mobile'><summary class='top-nav-mobile__summary'><span class='top-nav-mobile__summary-inner'><span class='top-nav-mobile__context'>ESP State</span><span class='top-nav-mobile__menu'><span class='top-nav-mobile__chev' aria-hidden='true'></span><span class='top-nav-mobile__menu-text'>Menu</span></span></span></summary><div class='top-nav-mobile__panel' role='group' aria-label='Portal pages'><a href='/status'>Spa Status</a><a href='/config'>Spa Config</a><a class='active' aria-current='page' href='/state'>ESP State</a><a href='/logs'>Logs</a><a href='/index.html'>Spa Website</a>" portalThemeToggleBtn "</div></details></nav><div class='portal-nav-scroll-sentinel' aria-hidden='true'></div>"

#define webMenuLogs "<nav aria-label='Portal navigation' class='portal-nav'><div class='portal-nav-bar'><div class='top-nav'><a href='/status'>Spa Status</a><a href='/config'>Spa Config</a><a href='/state'>ESP State</a><a class='active' aria-current='page' href='/logs'>Logs</a><a href='/index.html'>Spa Website</a></div><div class='portal-nav-util'>" portalThemeToggleBtn "</div></div><details class='top-nav-mobile'><summary class='top-nav-mobile__summary'><span class='top-nav-mobile__summary-inner'><span class='top-nav-mobile__context'>Logs</span><span class='top-nav-mobile__menu'><span class='top-nav-mobile__chev' aria-hidden='true'></span><span class='top-nav-mobile__menu-text'>Menu</span></span></span></summary><div class='top-nav-mobile__panel' role='group' aria-label='Portal pages'><a href='/status'>Spa Status</a><a href='/config'>Spa Config</a><a href='/state'>ESP State</a><a class='active' aria-current='page' href='/logs'>Logs</a><a href='/index.html'>Spa Website</a>" portalThemeToggleBtn "</div></details></nav><div class='portal-nav-scroll-sentinel' aria-hidden='true'></div>"

#ifdef spaEpaper
#define ePaper "<img class='panel-image' src='panel.jpg' alt='Spa Panel'>"
#else
#define ePaper ""
#endif

/** Local wall time for /status; invalid or epoch 0 → "Time not synced". */
static String statusFormatEpochLocalHuman(time_t t)
{
  if (t <= 0)
  {
    return String("Time not synced");
  }
  struct tm tmStore;
  struct tm *p = localtime_r(&t, &tmStore);
  if (!p)
  {
    return String("Time not synced");
  }
  char buf[32];
  if (strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", p) == 0)
  {
    return String("Time not synced");
  }
  return String(buf);
}

/** Primary local date/time for UI display. */
static String statusLastUpdateDisplayHtml(unsigned long epoch)
{
  time_t t = static_cast<time_t>(epoch);
  return statusFormatEpochLocalHuman(t);
}

/** Seconds since last spa status frame was applied (gateway clock); 0 if unknown. */
static unsigned long statusSnapshotAgeSec()
{
  time_t now = getTime();
  if (now <= 0 || spaStatusData.lastUpdate == 0)
  {
    return 0;
  }
  if ((time_t)spaStatusData.lastUpdate > now)
  {
    return 0;
  }
  return (unsigned long)(now - (time_t)spaStatusData.lastUpdate);
}

/** One-line subtitle: relative age + gateway-local time of last bus status apply. */
static String statusSnapshotSubtitle()
{
  if (spaStatusData.lastUpdate == 0)
  {
    return String("No spa status yet");
  }
  const String human = statusLastUpdateDisplayHtml(spaStatusData.lastUpdate);
  const unsigned long age = statusSnapshotAgeSec();
  if (age == 0 && getTime() <= 0)
  {
    return String("Snapshot at ") + human;
  }
  String out = "Updated ";
  if (age < 60UL)
  {
    out += String(age) + "s ago";
  }
  else if (age < 3600UL)
  {
    out += String(age / 60UL) + "m " + String(age % 60UL) + "s ago";
  }
  else
  {
    const unsigned long h = age / 3600UL;
    const unsigned long m = (age % 3600UL) / 60UL;
    out += String(h) + "h " + String(m) + "m ago";
  }
  out += " \xc2\xb7 ";
  out += human;
  return out;
}

static String webWallClockDisplayHtml(time_t t)
{
  if (t <= 0)
  {
    return statusLastUpdateDisplayHtml(0UL);
  }
  return statusLastUpdateDisplayHtml(static_cast<unsigned long>(t));
}

template <typename HtmlOut>
static void appendStatusKvRow(HtmlOut &html, const char *label, const String &value, const char *ddId = nullptr, const char *ddTitle = nullptr)
{
  html += "<div class=\"kv-row\"><dt>";
  html += label;
  html += "</dt><dd";
  if (ddId != nullptr && ddId[0] != '\0')
  {
    html += " id=\"";
    html += ddId;
    html += "\"";
  }
  if (ddTitle != nullptr && ddTitle[0] != '\0')
  {
    html += " title=\"";
    html += ddTitle;
    html += "\"";
  }
  html += ">";
  html += value;
  html += "</dd></div>";
}

/** Status byte9 bit1 (mask 0x02): panel 12h vs 24h clock display. */
static String statusPanelClockFormatLabel(uint8_t clockModeFromStatus)
{
  return ((clockModeFromStatus & 0x02) != 0) ? String("24-hour") : String("12-hour (AM/PM)");
}

/** Gateway wall clock as HH:MM for \"sync panel time\" (uses `getTime()` / local TZ). */
static String statusGatewayLocalTimeHHMM()
{
  time_t t = getTime();
  if (t <= 0)
  {
    return String("--:--");
  }
  struct tm tmStore;
  struct tm *p = localtime_r(&t, &tmStore);
  if (p == nullptr)
  {
    return String("--:--");
  }
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", p->tm_hour, p->tm_min);
  return String(buf);
}

static bool statusSpaConfigReady()
{
  return spaConfigurationData.lastUpdate != 0;
}

/** Configuration 0x2E: pump two-bit 0 = None (not installed). */
static bool statusPumpConfiguredAbsent(unsigned pumpId)
{
  if (!statusSpaConfigReady() || pumpId < 1 || pumpId > 6)
  {
    return false;
  }
  const uint8_t *p = &spaConfigurationData.pump1;
  return p[pumpId - 1] == 0;
}

static uint8_t statusPumpConfigSpeed(unsigned pumpId)
{
  if (!statusSpaConfigReady() || pumpId < 1 || pumpId > 6)
  {
    return 0;
  }
  const uint8_t *p = &spaConfigurationData.pump1;
  return p[pumpId - 1];
}

static uint8_t statusPumpRawState(unsigned pumpId)
{
  if (pumpId < 1 || pumpId > 6)
  {
    return 0;
  }
  const uint8_t *p = &spaStatusData.pump1;
  return p[pumpId - 1];
}

static bool statusPumpIsOn(unsigned pumpId)
{
  return statusPumpRawState(pumpId) > 0;
}

static String statusPumpDisplayState(unsigned pumpId)
{
  const uint8_t cfg = statusPumpConfigSpeed(pumpId);
  if (cfg == 1)
  {
    return getMapDescription(statusPumpIsOn(pumpId) ? 1 : 0, onOffMap);
  }
  return getMapDescription(statusPumpRawState(pumpId), pumpMap);
}

/** CSS suffix for `equip-cell--*`: off, low, high, on. Null when card is `equip-absent`. */
static const char *statusPumpEquipStateClass(unsigned pumpId, bool configuredAbsent)
{
  if (configuredAbsent)
  {
    return nullptr;
  }
  const uint8_t cfg = statusPumpConfigSpeed(pumpId);
  if (cfg == 1)
  {
    return statusPumpIsOn(pumpId) ? "on" : "off";
  }
  const uint8_t raw = statusPumpRawState(pumpId);
  if (raw == 0)
  {
    return "off";
  }
  if (raw == 1)
  {
    return "low";
  }
  return "high";
}

static const char *statusBinaryEquipStateClass(bool configuredAbsent, bool on)
{
  if (configuredAbsent)
  {
    return nullptr;
  }
  return on ? "on" : "off";
}

static void fillPumpDiagSnapshot(JsonObject obj)
{
  obj["statusLastUpdate"] = spaStatusData.lastUpdate;
  obj["pump1"] = spaStatusData.pump1;
  obj["pump2"] = spaStatusData.pump2;
  obj["pump3"] = spaStatusData.pump3;
  obj["pump4"] = spaStatusData.pump4;
  obj["pump5"] = spaStatusData.pump5;
  obj["pump6"] = spaStatusData.pump6;
  obj["pump1On"] = statusPumpIsOn(1);
  obj["pump2On"] = statusPumpIsOn(2);
  obj["pump3On"] = statusPumpIsOn(3);
  obj["pump4On"] = statusPumpIsOn(4);
  obj["pump5On"] = statusPumpIsOn(5);
  obj["pump6On"] = statusPumpIsOn(6);
  obj["light1"] = spaStatusData.light1 ? 1 : 0;
  obj["setTemp"] = spaStatusData.setTemp;
  obj["heatingState"] = spaStatusData.heatingState;
  if (spaStatusData.rawDataLength > 20)
  {
    JsonObject statusBytes = obj.createNestedObject("statusBytes");
    statusBytes["hf"] = spaStatusData.rawData[10];
    statusBytes["pp"] = spaStatusData.rawData[11];
    statusBytes["lf"] = spaStatusData.rawData[14];
    statusBytes["stRaw"] = spaStatusData.rawData[20];
  }
}

/** Configuration byte 2: 0 = None, 1 = Present. */
static bool statusLightConfiguredAbsent(unsigned lightId)
{
  if (!statusSpaConfigReady() || lightId < 1 || lightId > 2)
  {
    return false;
  }
  return lightId == 1 ? (spaConfigurationData.light1 == 0) : (spaConfigurationData.light2 == 0);
}

/** Two-bit 0 in config payload = not fitted (same convention as pumps). */
static bool statusCircConfiguredAbsent()
{
  return statusSpaConfigReady() && !spaConfigurationData.circulationPump;
}

static bool statusBlowerConfiguredAbsent()
{
  return statusSpaConfigReady() && !spaConfigurationData.blower;
}

static bool statusMisterConfiguredAbsent()
{
  return statusSpaConfigReady() && !spaConfigurationData.mister;
}

/** Configuration 0x2E pump two-bit: 0=None, 1=1-speed, 2=2-speed. */
static String spaConfigPumpInstallLabel(uint8_t cfg)
{
  switch (cfg)
  {
  case 0:
    return String("Not installed");
  case 1:
    return String("1-speed");
  case 2:
    return String("2-speed");
  default:
  {
    char buf[24];
    snprintf(buf, sizeof(buf), "Unknown (%u)", cfg);
    return String(buf);
  }
  }
}

static String spaConfigLightInstallLabel(uint8_t cfg)
{
  if (cfg == 0)
  {
    return String("Not installed");
  }
  if (cfg == 1)
  {
    return String("Installed");
  }
  char buf[24];
  snprintf(buf, sizeof(buf), "Unknown (%u)", cfg);
  return String(buf);
}

static String spaConfigLoadInstallLabel(bool present)
{
  return present ? String("Installed") : String("Not installed");
}

template <typename HtmlOut>
static void appendConfigEquipRow(HtmlOut &html, const char *load, const String &status, bool absent)
{
  html += "<tr";
  if (absent)
  {
    html += " class=\"config-equip-absent\" title=\"Not installed on this spa pack\"";
  }
  html += "><td>";
  html += load;
  html += "</td><td>";
  html += status;
  html += "</td></tr>";
}

template <typename HtmlOut>
static void appendStatusEquipCell(HtmlOut &html, const char *label, const String &value, bool configuredAbsent)
{
  if (configuredAbsent)
  {
    html += "<div class=\"equip-cell equip-absent\" title=\"Not installed (spa configuration)\"><div class=\"equip-label\">";
  }
  else
  {
    html += "<div class=\"equip-cell\"><div class=\"equip-label\">";
  }
  html += label;
  html += "</div><div class=\"equip-val\">";
  html += value;
  html += "</div></div>";
}

template <typename HtmlOut>
static void appendStatusControlCell(HtmlOut &html, const char *label, const char *equipKey, const String &value, bool configuredAbsent, int buttonCode, const char *desiredState, const char *equipStateClass)
{
  if (configuredAbsent)
  {
    html += "<div class=\"equip-cell equip-absent\" title=\"Not installed (spa configuration)\"";
  }
  else
  {
    html += "<div class=\"equip-cell";
    if (equipStateClass != nullptr && equipStateClass[0] != '\0')
    {
      html += " equip-cell--";
      html += equipStateClass;
    }
    html += "\"";
  }
  if (equipKey != nullptr && equipKey[0] != '\0')
  {
    html += " data-equip=\"";
    html += equipKey;
    html += "\"";
  }
  html += ">";
  html += "<div class=\"equip-label\">";
  html += label;
  html += "</div><div class=\"equip-val\" data-role=\"value\">";
  html += value;
  html += "</div>";

  if (!configuredAbsent && buttonCode > 0 && desiredState != nullptr)
  {
    html += "<div class=\"equip-actions\"><button class=\"equip-btn\" type=\"button\" data-button=\"";
    html += String(buttonCode);
    html += "\" data-state=\"";
    html += desiredState;
    html += "\" onclick=\"statusSendButton(this)\">Turn ";
    html += String(desiredState).equalsIgnoreCase("on") ? "On" : "Off";
    html += "</button></div>";
  }
  html += "</div>";
}

/** Spa status `tempScale`: 0 = Fahrenheit (1°F steps), 1 = Celsius (0.5°C steps). */
static bool statusSpaTempReady()
{
  return spaStatusData.lastUpdate != 0;
}

static String statusTempDegreeSuffixStr()
{
  if (!statusSpaTempReady())
  {
    return String("");
  }
  return spaStatusData.tempScale ? (String("\xc2\xb0") + "C") : (String("\xc2\xb0") + "F");
}

static String statusFormatTempValue(float v)
{
  if (!statusSpaTempReady())
  {
    return String("---");
  }
  if (spaStatusData.tempScale)
  {
    return String(v, 1);
  }
  return String(static_cast<long>(lroundf(v)));
}

static String statusFormattedTempWithUnit(float v)
{
  if (!statusSpaTempReady())
  {
    return String("---");
  }
  return statusFormatTempValue(v) + statusTempDegreeSuffixStr();
}

/** Stored high/low setpoint for a band; em dash when never populated (<=0). */
static String statusBandStoredSetpointText(float v)
{
  if (!statusSpaTempReady() || v <= 0.0f)
  {
    return String("\xe2\x80\x94");
  }
  return statusFormattedTempWithUnit(v);
}

static String statusFormatRuntimeHoursMinutes(unsigned long totalSeconds)
{
  const unsigned long totalMinutes = totalSeconds / 60UL;
  const unsigned long hours = totalMinutes / 60UL;
  const unsigned long minutes = totalMinutes % 60UL;

  String out = formatNumberWithCommas(hours);
  out += "h ";
  if (minutes < 10UL)
  {
    out += "0";
  }
  out += String(minutes);
  out += "m";
  return out;
}

/** Oldest-first comma list (matches chart / API order). */
static String temperatureHistoryOldestFirstString()
{
  String out;
  for (int i = TEMP_HISTORY_SLOTS - 1; i >= 0; i--)
  {
    if (out.length())
    {
      out += ", ";
    }
    out += String(tempHistoryData.samples[i]);
  }
  return out;
}

/** Same merge as ePaper `mergeGraphData`: history[22]..history[0] plus today (seconds); oldest-first. */
static String mergedDailySecondsHistoryString(const float *history, unsigned long todaySeconds)
{
  String out;
  for (int i = GRAPH_MAX_READINGS - 2; i >= 0; i--)
  {
    if (out.length())
    {
      out += ", ";
    }
    out += String(history[i]);
  }
  if (out.length())
  {
    out += ", ";
  }
  out += String(todaySeconds);
  out += " (today)";
  return out;
}

static void appendMergedDailySecondsHistory(JsonArray &arr, const float *history, unsigned long todaySeconds)
{
  for (int i = GRAPH_MAX_READINGS - 2; i >= 0; i--)
  {
    arr.add(history[i]);
  }
  arr.add(static_cast<float>(todaySeconds));
}

template <typename HtmlOut>
static void appendStatusHistoriesSection(HtmlOut &html)
{
  const unsigned long heatTodaySec = spaStatusData.heatOn ? spaStatusData.heatOn->today() : spaStatusData.heaterOnTimeToday;
  const unsigned long filterTodaySec = spaStatusData.filterOn ? spaStatusData.filterOn->today() : spaStatusData.filterOnTimeToday;

  html += "<div id=\"statusTempHistSection\" class=\"history-block status-temp-hist-anchor\">";
  html += "<h3>Temperature history</h3>";
  html += "<p class=\"chart-caption\">Last 24 hours, every 10 minutes (left = older, right = now). Held in RAM (survives soft reboot); power loss clears the chart until it refills.</p>";
  html += "<div class=\"chart-wrap\"><canvas id=\"statusTempHistChart\" height=\"140\" aria-label=\"Temperature history chart\"></canvas></div>";
  html += "<details class=\"history-raw\"><summary>Raw temperature values (oldest first, matches chart)</summary><pre>";
  html += temperatureHistoryOldestFirstString();
  html += "</pre></details></div>";

  html += "<div id=\"statusHeatHistSection\" class=\"history-block status-temp-hist-anchor\">";
  html += "<h3>Heater on-time history</h3>";
  html += "<p class=\"chart-caption\">Last 23 completed days plus today (in progress). Chart: minutes per day; rightmost point is today.</p>";
  html += "<div class=\"chart-wrap\"><canvas id=\"statusHeatHistChart\" height=\"140\" aria-label=\"Heater history chart\"></canvas></div>";
  html += "<details class=\"history-raw\"><summary>Raw heat history (seconds per day, oldest first)</summary><pre>";
  html += mergedDailySecondsHistoryString(spaStatusData.heatOn->history(), heatTodaySec);
  html += "</pre></details></div>";

  html += "<div class=\"history-block\"><h3>Filter on-time history</h3>";
  html += "<p class=\"chart-caption\">Last 23 completed days plus today (in progress). Chart: hours per day; rightmost point is today.</p>";
  html += "<div class=\"chart-wrap\"><canvas id=\"statusFilterHistChart\" height=\"140\" aria-label=\"Filter history chart\"></canvas></div>";
  html += "<details class=\"history-raw\"><summary>Raw filter history (seconds per day, oldest first)</summary><pre>";
  html += mergedDailySecondsHistoryString(spaStatusData.filterOn->history(), filterTodaySec);
  html += "</pre></details></div>";
}

void handleStatus(AsyncWebServerRequest *request)
{
  if (!portalLargePageTryBegin(request, "/status"))
  {
    return;
  }
  Log.verbose("[Web]: Request %s received from %p" CR, request->url().c_str(), request->client()->remoteIP());
  PortalHtmlChunks html;
  const char *statusStyle =
      "<style>"
      "html{scroll-behavior:smooth;}"
      ".status-page-head{display:flex;flex-wrap:wrap;align-items:baseline;justify-content:space-between;gap:10px 16px;margin:0 0 var(--space-3) 0;}"
      ".status-page-head .status-page-title{margin:0;}"
      ".status-page-title{color:var(--heading);font-size:1.1rem;line-height:1.3;}"
      ".status-snapshot-meta{margin:0;flex:1 1 220px;text-align:right;font-size:0.82rem;line-height:1.35;color:var(--muted);max-width:46em;}"
      "@media (max-width:520px){.status-snapshot-meta{text-align:left;flex:1 1 100%;}}"
      ".status-layout{display:grid;grid-template-columns:1fr;gap:var(--space-3);}"
      "@media (min-width:720px){.status-layout{grid-template-columns:1fr 1fr;}}"
      ".status-layout .panel{margin-bottom:0;}"
      ".status-span-full{grid-column:1/-1;}"
      ".status-layout h2{color:var(--heading);font-size:0.95rem;margin:0 0 var(--space-2) 0;font-weight:700;line-height:1.3;}"
      "dl.kv{margin:0;padding:0;}"
      "dl.kv .kv-row{display:grid;grid-template-columns:minmax(110px,42%) 1fr;gap:6px 12px;padding:var(--space-1) 0;"
      "border-bottom:1px dashed var(--border);align-items:start;}"
      "dl.kv .kv-row:last-child{border-bottom:none;}"
      "dl.kv dt{margin:0;font-weight:600;color:var(--muted);font-size:0.92rem;}"
      "dl.kv dd{margin:0;overflow-wrap:anywhere;word-break:break-word;}"
      ".kv-dd-with-inline-action{display:flex;flex-wrap:wrap;align-items:center;gap:8px;column-gap:10px;}"
      ".kv-dd-current-temp{flex-wrap:nowrap;}"
      ".kv-dd-current-temp > span{white-space:nowrap;}"
      ".status-temp-chart-link{color:var(--heading);display:inline-flex;align-items:center;vertical-align:middle;"
      "text-decoration:none;border-radius:6px;padding:3px;line-height:0;}"
      ".status-temp-chart-link:hover{background:var(--surface-2);}"
      ".status-temp-chart-link:focus-visible{outline:2px solid var(--focus);outline-offset:2px;}"
      ".status-temp-units-toggle{display:inline-flex;align-items:center;gap:0;overflow:hidden;border:1px solid var(--border);border-radius:8px;background:var(--panel);}"
      ".status-temp-units-toggle button{flex:0 0 auto;min-height:0;padding:1px 5px;border:0;border-right:1px solid var(--border);background:var(--panel);color:var(--muted);font-size:.62rem;line-height:1;cursor:pointer;}"
      ".status-temp-units-toggle button:last-child{border-right:0;}"
      ".status-temp-units-toggle button:hover{background:var(--bg);color:var(--text);}"
      ".status-temp-units-toggle button:focus-visible{outline:2px solid var(--focus);outline-offset:2px;position:relative;z-index:1;}"
      ".status-temp-units-toggle button:disabled{background:var(--focus);color:#fff;cursor:default;opacity:1;}"
      ".heat-panel-head{display:flex;flex-wrap:wrap;justify-content:space-between;align-items:flex-start;gap:10px;margin:0 0 6px 0;}"
      ".heat-panel-head h2{margin:0;}"
      ".heat-hint{font-size:0.82rem;color:var(--muted);margin:0 0 12px 0;line-height:1.45;max-width:52em;}"
      ".heat-hero-grid{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin:0 0 12px 0;align-items:stretch;}"
      "@media (max-width:560px){.heat-hero-grid{grid-template-columns:1fr;}#statusHeatHero{order:-1;}}"
      ".heat-hero{display:flex;align-items:center;gap:12px;padding:12px 14px;border-radius:10px;border:1px solid var(--border);margin:0;background:var(--surface-2);min-width:0;}"
      ".heat-hero-icon{flex-shrink:0;line-height:0;color:var(--heading);}"
      ".heat-hero--ok{border-color:#b8cfe8;background:#f2f7fc;}.heat-hero--ok .heat-hero-icon{color:#0f4a87;}"
      ".heat-hero--init{border-color:#e6c200;background:#fffbeb;}.heat-hero--init .heat-hero-icon{color:#b8860b;}"
      ".heat-hero--alert{border-color:#e57373;background:#fff5f5;}.heat-hero--alert .heat-hero-icon{color:#c62828;}"
      ".kv-row--alert dt,.kv-row--alert dd{color:#c62828;font-weight:600;}"
      ".kv-row--alert-warn dt,.kv-row--alert-warn dd{color:#8d6e00;font-weight:600;}"
      ".status-reminder-banner{display:none;align-items:flex-start;gap:12px;margin:0 0 var(--space-3) 0;"
      "padding:14px 16px;border-radius:10px;border:1px solid;}"
      ".status-reminder-banner.is-active{display:flex;}"
      ".status-reminder-banner--warn{border-color:#e6c200;background:#fffbeb;color:#5c4a00;}"
      ".status-reminder-banner--fault{border-color:#e57373;background:#fff5f5;color:#b71c1c;}"
      ".status-reminder-banner-icon{flex-shrink:0;line-height:0;margin-top:1px;}"
      ".status-reminder-banner--warn .status-reminder-banner-icon{color:#b8860b;}"
      ".status-reminder-banner--fault .status-reminder-banner-icon{color:#c62828;}"
      ".status-reminder-banner-label{font-size:0.78rem;font-weight:600;text-transform:uppercase;"
      "letter-spacing:0.03em;opacity:0.9;margin:0 0 4px 0;}"
      ".status-reminder-banner-val{font-size:1.15rem;font-weight:700;line-height:1.3;margin:0;}"
      ".status-reminder-banner-hint{font-size:0.82rem;line-height:1.4;margin:6px 0 0 0;opacity:0.92;}"
      ".heat-hero--heat-idle{border-color:#dde2e8;background:#eef1f4;}.heat-hero--heat-idle .heat-hero-icon{color:#5f6c7b;}"
      ".heat-hero--heat-on{border-color:#ffab91;background:#ffe8e0;}.heat-hero--heat-on .heat-hero-icon{color:#bf360c;}"
      ".heat-hero--heat-alt{border-color:#ffe082;background:#fff8e1;}.heat-hero--heat-alt .heat-hero-icon{color:#8d6e00;}"
      ".heat-hero--heat-reserved{border-color:#cfd8dc;background:#eceff1;}.heat-hero--heat-reserved .heat-hero-icon{color:#546e7a;}"
      "#statusHeatHero .heat-hero-icon{color:#d32f2f;}"
      "@keyframes heatHeroPulse{0%,100%{box-shadow:0 0 0 0 rgba(211,47,47,0);}"
      "50%{box-shadow:0 0 22px 8px rgba(211,47,47,0.42);}}"
      ".heat-hero--heat-on,.heat-hero--heat-alt{animation:heatHeroPulse 3.2s ease-in-out infinite;}"
      "@media (prefers-reduced-motion:reduce){.heat-hero--heat-on,.heat-hero--heat-alt{animation:none;}}"
      ".heat-hero-label{font-size:0.78rem;font-weight:600;color:var(--muted);text-transform:uppercase;letter-spacing:0.03em;}"
      ".heat-hero-val{font-size:1.12rem;font-weight:700;margin-top:2px;line-height:1.25;}"
      ".heat-hero-val--emph{font-size:1.22rem;}"
      ".heat-chips{display:flex;flex-wrap:wrap;gap:8px;margin:0 0 12px 0;align-items:center;}"
      ".heat-chip{display:inline-flex;align-items:center;gap:6px;padding:6px 12px;border-radius:999px;font-size:0.84rem;font-weight:600;border:1px solid var(--border);background:#fff;}"
      ".heat-chip svg{flex-shrink:0;}"
      ".heat-chip--heat-idle{color:#5f6c7b;background:#eef1f4;border-color:#dde2e8;}"
      ".heat-chip--heat-on{color:#8b2500;background:#ffe8e0;border-color:#ffab91;}"
      ".heat-chip--heat-alt{color:#6d4c00;background:#fff8e1;border-color:#ffe082;}"
      ".heat-chip--need-yes{color:#b71c1c;background:#ffebee;border-color:#ffcdd2;}"
      ".heat-chip--need-no{color:#455a64;background:#eceff1;border-color:#cfd8dc;}"
      ".heat-chip-lbl{font-weight:500;opacity:0.88;margin-right:2px;}"
      "details.heat-raw{margin-top:10px;}details.heat-raw summary{cursor:pointer;font-size:0.84rem;color:var(--muted);font-weight:600;}"
      "details.heat-raw pre{margin:8px 0 0 0;padding:8px 10px;background:var(--surface-2);border:1px solid var(--border);border-radius:8px;font-size:0.78rem;line-height:1.5;font-family:ui-monospace,Courier,monospace;}"
      ".equip-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:8px;margin-top:var(--space-2);}"
      ".equip-cell{border:1px solid var(--border);border-radius:8px;padding:8px 10px 8px 14px;background:var(--surface-2);}"
      ".equip-cell--off{background:#f4f6f8;border-color:#dde3e9;box-shadow:inset 4px 0 0 0 #b0bec5;}"
      ".equip-cell--low{background:#fff8e6;border-color:#e6c86a;box-shadow:inset 4px 0 0 0 #e6a000;}"
      ".equip-cell--high,.equip-cell--on{background:#e8f5f0;border-color:#7ebda3;box-shadow:inset 4px 0 0 0 #2e8b6e;}"
      ".equip-cell.equip-absent{opacity:0.58;background:#eef1f4;color:var(--muted);border-color:#dde2e8;box-shadow:inset 4px 0 0 0 #c5ced6;}"
      ".equip-cell.equip-absent .equip-label{color:#7a8794;}"
      ".equip-cell.equip-absent .equip-val{font-weight:500;color:#5f6c7b;}"
      ".equip-grid.status-equip-hide-absent .equip-cell.equip-absent{display:none;}"
      ".status-equip-head{display:flex;flex-wrap:wrap;align-items:center;justify-content:space-between;gap:8px 16px;}"
      ".status-equip-head h2{margin:0;}"
      ".status-equip-show-absent-lbl{display:inline-flex;align-items:center;gap:8px;font-size:14px;color:var(--muted);cursor:pointer;user-select:none;}"
      ".status-equip-show-absent-lbl input{margin:0;width:1rem;height:1rem;cursor:pointer;}"
      ".equip-label{font-size:0.82rem;color:var(--muted);font-weight:600;}"
      ".equip-val{font-weight:600;margin-top:2px;line-height:1.35;}"
      ".equip-actions{margin-top:8px;}"
      ".equip-btn{background:var(--focus);color:#fff;border:none;border-radius:6px;padding:6px 10px;cursor:pointer;font-size:.84rem;}"
      ".equip-btn:disabled{opacity:.55;cursor:not-allowed;}"
      ".range-bands{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin:10px 0;}"
      "#statusBandLow{grid-column:1;grid-row:1;}#statusBandHigh{grid-column:2;grid-row:1;}"
      "@media (max-width:520px){.range-bands{grid-template-columns:1fr;}#statusBandLow,#statusBandHigh{grid-column:auto;grid-row:auto;}}"
      ".range-band{border:1px solid var(--border);border-radius:8px;padding:10px 12px;background:var(--surface-2);}"
      "button.range-band{display:block;width:100%;margin:0;text-align:start;font:inherit;color:inherit;"
      "appearance:none;-webkit-appearance:none;border-radius:8px;cursor:pointer;transition:border-color .15s,box-shadow .15s,transform .12s;}"
      "button.range-band:not(:disabled):hover{border-color:#8b96a3;box-shadow:0 2px 8px rgba(0,0,0,.08);transform:translateY(-1px);}"
      "button.range-band:not(:disabled):active{transform:translateY(0);box-shadow:0 1px 3px rgba(0,0,0,.06);}"
      "button.range-band:focus-visible{outline:2px solid var(--focus);outline-offset:2px;z-index:1;position:relative;}"
      "button.range-band:disabled{cursor:default;opacity:1;}"
      ".range-band-active-low{border-color:var(--focus);background:#e8f0fa;box-shadow:0 0 0 2px rgba(15,74,135,0.12);}"
      ".range-band-active-high{border-color:#b71c1c;background:#fde8e8;box-shadow:0 0 0 2px rgba(183,28,28,0.16);}"
      ".range-band-title{font-size:0.82rem;font-weight:600;color:var(--muted);margin:0 0 6px 0;display:flex;align-items:center;justify-content:space-between;gap:8px;width:100%;}"
      ".range-band-indicator{font-size:0.95rem;line-height:1;color:#8b96a3;}"
      ".range-band-active-low .range-band-indicator,.range-band-active-high .range-band-indicator{color:var(--text);}"
      ".range-band-temp{font-size:1.15rem;font-weight:700;margin:0;line-height:1.25;display:block;width:100%;}"
      ".range-hint{font-size:0.82rem;color:var(--muted);margin:8px 0 0 0;line-height:1.35;}"
      ".status-control-row{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-top:10px;}"
      ".status-control-row input{border:1px solid var(--border);border-radius:6px;padding:6px 8px;min-width:90px;}"
      ".status-control-result{margin-top:8px;font-size:.84rem;color:var(--muted);}"
      ".status-temp-hist-anchor{scroll-margin-top:16px;}"
      ".history-block{margin-top:var(--space-2);}"
      ".history-block h3{margin:0 0 6px 0;font-size:0.88rem;color:var(--muted);font-weight:600;}"
      ".history-block pre{margin:0;padding:10px;background:var(--surface-2);border:1px solid var(--border);border-radius:8px;"
      "font-size:0.8rem;line-height:1.45;overflow-x:auto;white-space:pre-wrap;word-break:break-word;font-family:ui-monospace,Courier,monospace;}"
      ".chart-caption{font-size:0.82rem;color:var(--muted);margin:0 0 6px 0;line-height:1.35;}"
      ".history-block .chart-wrap{max-width:100%;overflow:hidden;}"
      ".history-raw{margin-top:8px;}details.history-raw summary{cursor:pointer;font-size:0.88rem;color:var(--muted);font-weight:600;}"
      "</style>";

  // Build `<head>` with sequential appends (see `appendPortalHead`).
  html = F("<html>");
  const bool wroteOpeningTag = html.healthy() && html.length() >= 6;
  const bool sawPortalCss = appendPortalHead(html, "Spa Status");
  html += statusStyle;
  html += F("<body><a class='skip-link' href='#mainContent'>Skip to main content</a><div class='page'>");
  html += webMenuStatus;
  html += F("<main id='mainContent'>");
  html += ePaper;
  html += F("<div class=\"status-page-head\"><h1 class=\"status-page-title\">Spa Status</h1>"
            "<p class=\"status-snapshot-meta\" id=\"statusSnapshotMeta\" title=\"Last spa status frame applied (gateway local time)\">");
  html += statusSnapshotSubtitle();
  html += F("</p></div>");
  {
    const String reminderTxt =
        spaReminderText(spaStatusData.reminderType, spaStatusData.spaState);
    const bool reminderActive = spaReminderIsActive(
        spaStatusData.reminderType, spaStatusData.spaState, spaStatusData.initMode);
    const bool reminderFault = spaReminderIsFault(spaStatusData.reminderType);
    html += "<div id=\"statusReminderBanner\" class=\"status-reminder-banner";
    if (reminderActive)
    {
      html += " is-active ";
      html += reminderFault ? "status-reminder-banner--fault" : "status-reminder-banner--warn";
    }
    html += "\" role=\"alert\" aria-live=\"polite\">";
    html += "<span class=\"status-reminder-banner-icon\" aria-hidden=\"true\">";
    html += "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"28\" height=\"28\" viewBox=\"0 0 24 24\" fill=\"none\" "
            "stroke=\"currentColor\" stroke-width=\"1.75\" stroke-linecap=\"round\" stroke-linejoin=\"round\">";
    html += "<path d=\"M10.29 3.86 1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z\"/>";
    html += "<line x1=\"12\" y1=\"9\" x2=\"12\" y2=\"13\"/><line x1=\"12\" y1=\"17\" x2=\"12.01\" y2=\"17\"/></svg></span>";
    html += "<div><p class=\"status-reminder-banner-label\">Panel reminder</p>";
    html += "<p id=\"statusReminderBannerVal\" class=\"status-reminder-banner-val\">";
    html += reminderActive ? reminderTxt : "";
    html += "</p><p id=\"statusReminderBannerHint\" class=\"status-reminder-banner-hint\">";
    html += reminderActive
                ? spaReminderHintText(spaStatusData.reminderType, spaStatusData.spaState)
                : "";
    html += "</p></div></div>";
    html += F("<div class=\"status-layout\">");
  }

  {
    float setMin = 50.0f;
    float setMax = 104.0f;
    spaProtocolActiveSetpointBand(setMin, setMax);
    const String setMinStr = String(setMin, spaStatusData.tempScale ? 1 : 0);
    const String setMaxStr = String(setMax, spaStatusData.tempScale ? 1 : 0);
    const bool activeHigh = (spaStatusData.tempRange != 0);
    html += "<section class=\"panel\"><h2>Temperatures</h2><dl class=\"kv\">";
    html += "<div class=\"kv-row\"><dt>Current Temp</dt><dd class=\"kv-dd-with-inline-action kv-dd-current-temp\"><span>";
    html += statusFormattedTempWithUnit(spaStatusData.currentTemp);
    html += "</span><a href=\"#statusTempHistSection\" class=\"status-temp-chart-link\" data-history-anchor=\"statusTempHistSection\" title=\"Jump to temperature chart\" aria-label=\"Jump to temperature chart\">";
    html += "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"18\" height=\"18\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\" aria-hidden=\"true\">";
    html += "<path d=\"M4 19V5\"/><path d=\"M4 19h16\"/><path d=\"M8 17V9\"/><path d=\"M12 17v-5\"/><path d=\"M16 17V6\"/><path d=\"M20 17v-9\"/></svg></a>";
    html += "<span class=\"status-temp-units-toggle\" role=\"group\" aria-label=\"Temperature units\">";
    html += "<button id=\"statusTempUnitsToggleC\" type=\"button\" onclick=\"statusSendTempUnits('C')\"";
    html += spaStatusData.tempScale ? " disabled" : "";
    html += " title=\"Use Celsius\">C</button>";
    html += "<button id=\"statusTempUnitsToggleF\" type=\"button\" onclick=\"statusSendTempUnits('F')\"";
    html += spaStatusData.tempScale ? "" : " disabled";
    html += " title=\"Use Fahrenheit\">F</button>";
    html += "</span></dd></div>";
    html += "</dl>";
    html += "<div class=\"range-bands\" role=\"group\" aria-label=\"Temperature range setpoints\">";
    html += "<button type=\"button\" id=\"statusBandHigh\" data-range-band=\"high\" class=\"range-band";
    html += activeHigh ? " range-band-active-high" : "";
    html += "\" onclick=\"statusSendButton(this)\" data-button=\"80\" data-state=\"on\" aria-label=\"Use high temperature range\" aria-pressed='";
    html += activeHigh ? "true" : "false";
    html += "' title=\"Switch to high range\"";
    html += activeHigh ? " disabled" : "";
    html += "><span class=\"range-band-title\"><span>High range setpoint</span><span class=\"range-band-indicator\" aria-hidden=\"true\">▲</span></span><span id=\"statusBandHighVal\" class=\"range-band-temp\">";
    html += statusBandStoredSetpointText(spaStatusData.highSetTemp);
    html += "</span></button><button type=\"button\" id=\"statusBandLow\" data-range-band=\"low\" class=\"range-band";
    html += activeHigh ? "" : " range-band-active-low";
    html += "\" onclick=\"statusSendButton(this)\" data-button=\"80\" data-state=\"off\" aria-label=\"Use low temperature range\" aria-pressed='";
    html += activeHigh ? "false" : "true";
    html += "' title=\"Switch to low range\"";
    html += activeHigh ? "" : " disabled";
    html += "><span class=\"range-band-title\"><span>Low range setpoint</span><span class=\"range-band-indicator\" aria-hidden=\"true\">▼</span></span><span id=\"statusBandLowVal\" class=\"range-band-temp\">";
    html += statusBandStoredSetpointText(spaStatusData.lowSetTemp);
    html += "</span></button></div>";
    html += "<p class=\"range-hint\">Click a range above to switch. Set temp applies to the highlighted range only.</p>";
    html += "<div class=\"status-control-row\"><label for=\"statusSetTempInput\" class=\"equip-label\">Set temp <span id=\"statusSetTempScopeLabel\">";
    html += activeHigh ? "(high range)" : "(low range)";
    html += "</span></label>";
    html += "<input id=\"statusSetTempInput\" type=\"number\" min=\"";
    html += setMinStr;
    html += "\" max=\"";
    html += setMaxStr;
    html += "\" step=\"";
    html += (spaStatusData.tempScale ? "0.5" : "1");
    html += "\" value=\"";
    html += String(spaStatusData.setTemp, spaStatusData.tempScale ? 1 : 0);
    html += "\" />";
    html += "<button class=\"equip-btn\" type=\"button\" onclick=\"statusSendSetTemp()\">Send</button></div>";
    html += "<div id=\"statusSetTempResult\" class=\"status-control-result\"></div>";
    html += "<div id=\"statusTempUnitsResult\" class=\"status-control-result\"></div>";
    html += "</section>";
  }

  {
    const String spaStateTxt = getMapDescription(spaStatusData.spaState, spaStateMap);
    const String initTxt = getMapDescription(spaStatusData.initMode, initModeMap);
    const String heatModeTxt = getMapDescription(spaStatusData.heatingMode, heatingModeMap);
    const String heatStateTxt = getMapDescription(spaStatusData.heatingState, heatingStateMap);
    const uint8_t ss = spaStatusData.spaState;
    const uint8_t im = spaStatusData.initMode;
    const uint8_t hs = spaStatusData.heatingState;
    String heroClass = "heat-hero heat-hero--ok";
    if (im == 2)
    {
      heroClass = "heat-hero heat-hero--alert";
    }
    else if (ss == 1 || im == 1)
    {
      heroClass = "heat-hero heat-hero--init";
    }
    String heatHeroClass = "heat-hero heat-hero--heat-idle";
    if (hs == 1)
    {
      heatHeroClass = "heat-hero heat-hero--heat-on";
    }
    else if (hs == 2)
    {
      heatHeroClass = "heat-hero heat-hero--heat-alt";
    }
    else if (hs == 3)
    {
      heatHeroClass = "heat-hero heat-hero--heat-reserved";
    }
    html += "<section class=\"panel\"><div class=\"heat-panel-head\"><h2>Spa and heating</h2>";
    html += "<a href=\"#statusHeatHistSection\" class=\"status-temp-chart-link\" data-history-anchor=\"statusHeatHistSection\" title=\"Jump to heater on-time chart\" aria-label=\"Jump to heater on-time chart\">";
    html += "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"18\" height=\"18\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\" aria-hidden=\"true\">";
    html += "<path d=\"M4 19V5\"/><path d=\"M4 19h16\"/><path d=\"M8 17V9\"/><path d=\"M12 17v-5\"/><path d=\"M16 17V6\"/><path d=\"M20 17v-9\"/></svg></a></div>";
    html += "<p class=\"heat-hint\"><b>Heating mode</b> is what the spa is set up for (ready/rest). <b>Heater state</b> is what the heater is doing right now (idle vs actively heating).</p>";
    html += "<div class=\"heat-hero-grid\"><div id=\"statusSpaHero\" class=\"";
    html += heroClass;
    html += "\"><span class=\"heat-hero-icon\" aria-hidden=\"true\">";
    html += "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"28\" height=\"28\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"1.75\" stroke-linecap=\"round\" stroke-linejoin=\"round\">";
    html += "<path d=\"M14 14.76V3.5a2.5 2.5 0 0 0-5 0v11.26a4.5 4.5 0 1 0 5 0z\"/></svg></span>";
    html += "<div><div class=\"heat-hero-label\">Spa State</div><div id=\"statusSpaStateHero\" class=\"heat-hero-val\">";
    html += spaStateTxt;
    html += "</div></div></div><div id=\"statusHeatHero\" class=\"";
    html += heatHeroClass;
    html += "\"><span class=\"heat-hero-icon\" aria-hidden=\"true\">";
    html += "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"28\" height=\"28\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"1.75\" stroke-linecap=\"round\" stroke-linejoin=\"round\" aria-hidden=\"true\">";
    html += "<path d=\"M8.5 14.5A2.5 2.5 0 0 0 11 12c0-1.38-.5-2-1-3-1.072-2.143-.224-4.054 2-6 .5 2.5 2 4.9 4 6.5 2 1.6 3 3.5 3 5.5a7 7 0 1 1-14 0c0-1.153.433-2.294 1-3a2.5 2.5 0 0 0 2.5 2.5z\"/></svg></span>";
    html += "<div><div class=\"heat-hero-label\">Heater State</div><div id=\"statusHeatStateHero\" class=\"heat-hero-val heat-hero-val--emph\">";
    html += heatStateTxt;
    html += "</div></div></div></div><dl class=\"kv\">";
    html += "<div class=\"kv-row\"><dt>Init mode</dt><dd id=\"statusInitModeVal\">";
    html += initTxt;
    html += "</dd></div><div class=\"kv-row\"><dt>Heating mode</dt><dd id=\"statusHeatingModeVal\">";
    html += heatModeTxt;
    html += "</dd></div></dl>";
    html += "<details class=\"heat-raw\"><summary>Raw status codes</summary><pre id=\"statusHeatRawPre\">spaState=";
    html += String(spaStatusData.spaState);
    html += " initMode=";
    html += String(spaStatusData.initMode);
    html += " heatingMode=";
    html += String(spaStatusData.heatingMode);
    html += " heatingState=";
    html += String(spaStatusData.heatingState);
    html += " needsHeat=";
    html += String(spaStatusData.needsHeat ? 1 : 0);
    html += "</pre></details></section>";
  }

  html += "<section class=\"panel status-span-full\"><div class=\"status-equip-head\"><h2>Equipment</h2>";
  html += "<label class=\"status-equip-show-absent-lbl\"><input type=\"checkbox\" id=\"statusEquipShowAbsent\" "
          "onchange=\"statusToggleEquipAbsent(this.checked)\" /> Show not installed</label></div>";
  html += "<div class=\"equip-grid status-equip-hide-absent\" id=\"statusEquipGrid\">";
  appendStatusControlCell(html, "Pump 1", "pump1", statusPumpDisplayState(1), statusPumpConfiguredAbsent(1), 4, statusPumpIsOn(1) ? "off" : "on", statusPumpEquipStateClass(1, statusPumpConfiguredAbsent(1)));
  appendStatusControlCell(html, "Pump 2", "pump2", statusPumpDisplayState(2), statusPumpConfiguredAbsent(2), 5, statusPumpIsOn(2) ? "off" : "on", statusPumpEquipStateClass(2, statusPumpConfiguredAbsent(2)));
  appendStatusControlCell(html, "Pump 3", "pump3", statusPumpDisplayState(3), statusPumpConfiguredAbsent(3), 6, statusPumpIsOn(3) ? "off" : "on", statusPumpEquipStateClass(3, statusPumpConfiguredAbsent(3)));
  appendStatusControlCell(html, "Pump 4", "pump4", statusPumpDisplayState(4), statusPumpConfiguredAbsent(4), 7, statusPumpIsOn(4) ? "off" : "on", statusPumpEquipStateClass(4, statusPumpConfiguredAbsent(4)));
  appendStatusControlCell(html, "Pump 5", "pump5", statusPumpDisplayState(5), statusPumpConfiguredAbsent(5), 8, statusPumpIsOn(5) ? "off" : "on", statusPumpEquipStateClass(5, statusPumpConfiguredAbsent(5)));
  appendStatusControlCell(html, "Pump 6", "pump6", statusPumpDisplayState(6), statusPumpConfiguredAbsent(6), 9, statusPumpIsOn(6) ? "off" : "on", statusPumpEquipStateClass(6, statusPumpConfiguredAbsent(6)));
  appendStatusControlCell(html, "Circulation Pump", "circ", getMapDescription(spaStatusData.circ, onOffMap), statusCircConfiguredAbsent(), 0, nullptr, statusBinaryEquipStateClass(statusCircConfiguredAbsent(), spaStatusData.circ != 0));
  appendStatusControlCell(html, "Blower", "blower", String(spaBlowerBinaryLabel(spaStatusData.blower)), statusBlowerConfiguredAbsent(), 12, spaStatusData.blower == 0 ? "on" : "off", statusBinaryEquipStateClass(statusBlowerConfiguredAbsent(), spaStatusData.blower != 0));
  appendStatusControlCell(html, "Light 1", "light1", getMapDescription(spaStatusData.light1, onOffMap), statusLightConfiguredAbsent(1), 17, spaStatusData.light1 ? "off" : "on", statusBinaryEquipStateClass(statusLightConfiguredAbsent(1), spaStatusData.light1 != 0));
  appendStatusControlCell(html, "Light 2", "light2", getMapDescription(spaStatusData.light2, onOffMap), statusLightConfiguredAbsent(2), 18, spaStatusData.light2 ? "off" : "on", statusBinaryEquipStateClass(statusLightConfiguredAbsent(2), spaStatusData.light2 != 0));
  appendStatusControlCell(html, "Mister", "mister", getMapDescription(spaStatusData.mister, onOffMap), statusMisterConfiguredAbsent(), 14, spaStatusData.mister ? "off" : "on", statusBinaryEquipStateClass(statusMisterConfiguredAbsent(), spaStatusData.mister != 0));
  html += "</div><div id=\"statusButtonResult\" class=\"status-control-result\"></div></section>";

  {
    String clockRawTitle = String("Raw status flag (status byte 9 & 0x02): ") + String(spaStatusData.clockMode);
    html += "<section class=\"panel\"><h2>Panel clock and filter cycles</h2>";
    html += "<p class=\"chart-caption\">Times are the <b>spa panel clock</b> from RS485 status (not the ESP clock on <a href='/state'>/state</a>). "
            "<b>Panel clock format</b> is how the physical panel shows time (12h vs 24h). "
            "<b>Filter cycle (status)</b> is which programmed daily filter window the controller reports as active; schedule start/duration is on <a href='/config'>/config</a>.</p>";
    html += "<dl class=\"kv\">";
    appendStatusKvRow(html, "Panel time", String(spaStatusData.time), "statusPanelTimeVal", nullptr);
    html += "<div class=\"kv-row\"><dt>Panel clock format</dt><dd class=\"kv-dd-with-inline-action\"><span id=\"statusClockFormatVal\" title=\"";
    html += clockRawTitle;
    html += "\">";
    html += statusPanelClockFormatLabel(spaStatusData.clockMode);
    html += "</span><span class=\"status-temp-units-toggle\" role=\"group\" aria-label=\"Panel clock format\">";
    html += "<button id=\"statusClockFormat12Btn\" type=\"button\" onclick=\"statusSendTimeFormat(12)\"";
    html += (spaStatusData.clockMode & 0x02) ? "" : " disabled";
    html += " title=\"Use 12-hour clock\">12</button>";
    html += "<button id=\"statusClockFormat24Btn\" type=\"button\" onclick=\"statusSendTimeFormat(24)\"";
    html += (spaStatusData.clockMode & 0x02) ? " disabled" : "";
    html += " title=\"Use 24-hour clock\">24</button>";
    html += "</span></dd></div>";
    appendStatusKvRow(html, "Filter cycle (status)", String(getMapDescription(spaStatusData.filterMode, filterModeMap)), "statusFilterModeVal", nullptr);
    html += "</dl>";
    html += "<p class=\"range-hint\" style=\"margin-top:10px\">Set panel clock sends Balboa <code>0x21</code> using the current 12h/24h format flag from status.</p>";
#ifdef LOCAL_CLIENT
#if AUTO_SYNC_PANEL_CLOCK
    html += "<p class=\"range-hint\">Auto-sync on boot: <b>enabled</b>. After Wi-Fi/NTP is up, the gateway may set the panel clock when drift exceeds ";
    html += String(AUTO_SYNC_PANEL_CLOCK_THRESHOLD_MIN);
    html += " minutes. Toggle in <code>config.h</code> (<code>AUTO_SYNC_PANEL_CLOCK</code>, then reflash).</p>";
#else
    html += "<p class=\"range-hint\">Auto-sync on boot: <b>disabled</b>. Set <code>AUTO_SYNC_PANEL_CLOCK</code> to <code>1</code> in <code>config.h</code> and reflash to sync the panel from gateway time after power loss (when drift exceeds the configured threshold).</p>";
#endif
#endif
    html += "<div class=\"status-control-row\"><label for=\"statusPanelTimeInput\" class=\"equip-label\">Set panel time</label>";
    html += "<input id=\"statusPanelTimeInput\" type=\"time\" step=\"60\" value=\"";
    html += String(spaStatusData.time);
    html += "\" />";
    html += "<button class=\"equip-btn\" type=\"button\" onclick=\"statusSendPanelTime()\">Send to spa</button>";
    html += "<button class=\"equip-btn\" type=\"button\" onclick=\"statusSyncPanelTimeFromGateway()\">Sync from gateway</button></div>";
    html += "<div id=\"statusSystemTimeResult\" class=\"status-control-result\"></div>";
    html += "<div id=\"statusTimeFormatResult\" class=\"status-control-result\"></div></section>";
  }

  html += "<section class=\"panel\"><h2>Panel and flags</h2><dl class=\"kv\">";
  appendStatusKvRow(html, "Panel Locked", getMapDescription(spaStatusData.panelLocked, lockedMap));
  appendStatusKvRow(html, "Settings Lock", getMapDescription(spaStatusData.settingsLock, lockedMap));
  {
    const String reminderTxt =
        spaReminderText(spaStatusData.reminderType, spaStatusData.spaState);
    html += "<div class=\"kv-row";
    if (spaReminderIsActive(
            spaStatusData.reminderType, spaStatusData.spaState, spaStatusData.initMode))
    {
      html += spaReminderIsFault(spaStatusData.reminderType) ? " kv-row--alert"
                                                             : " kv-row--alert-warn";
    }
    html += "\" id=\"statusReminderRow\"><dt>Reminder</dt><dd id=\"statusReminderVal\">";
    html += reminderTxt;
    html += "</dd></div>";
  }
  appendStatusKvRow(html, "M8 Cycle Time", String(spaStatusData.m8CycleTime));
  appendStatusKvRow(html, "Notification", String(spaStatusData.notification));
  appendStatusKvRow(html, "Flags 19", String(spaStatusData.flags19));
  html += "</dl></section>";

  html += "<section class=\"panel\"><h2>Run times</h2><dl class=\"kv\">";
  appendStatusKvRow(html, "Heater On Time Today", statusFormatRuntimeHoursMinutes(spaStatusData.heaterOnTimeToday));
  appendStatusKvRow(html, "Heater On Time Yesterday", statusFormatRuntimeHoursMinutes(spaStatusData.heaterOnTimeYesterday));
  appendStatusKvRow(html, "Filter On Time Today", statusFormatRuntimeHoursMinutes(spaStatusData.filterOnTimeToday));
  appendStatusKvRow(html, "Filter On Time Yesterday", statusFormatRuntimeHoursMinutes(spaStatusData.filterOnTimeYesterday));
  html += "</dl></section>";

  html += "<section class=\"panel status-span-full\"><h2>Histories</h2>";
  html += "<p class=\"chart-caption\">Load this on demand to keep first render fast on weak Wi-Fi.</p>";
  html += "<button class=\"equip-btn\" type=\"button\" id=\"statusLoadHistoriesBtn\">Load history charts</button>";
  html += "<div id=\"statusHistoriesResult\" class=\"status-control-result\"></div>";
  html += "<div id=\"statusHistoriesContainer\"></div>";
  html += "</section>";

  html += "<script>"
          "async function statusSendSci(payload){"
          "const body='<sci_request version=\"1.0\"><data_service><targets><device id=\"00 11 22 33 44 55 66 77\"/></targets><requests>'+payload+'</requests></data_service></sci_request>';"
          "const r=await fetch('/devices/sci',{method:'POST',headers:{'Content-Type':'application/xml'},body});"
          "return await r.text();"
          "}"
          "function statusBackoffMs(base,max,fails){var e=Math.min(max,base*Math.pow(2,Math.min(6,fails)));var j=Math.floor(Math.random()*Math.max(250,Math.floor(e*0.35)));return Math.min(max,e+j);}"
          "async function statusFetchJson(url,timeoutMs){const ctl=new AbortController();const t=setTimeout(function(){ctl.abort();},timeoutMs||5000);"
          "try{const r=await fetch(url,{cache:'no-store',signal:ctl.signal});if(!r.ok)throw new Error('http_'+r.status);return await r.json();}finally{clearTimeout(t);}}"
          "async function statusFetchControls(){return await statusFetchJson('/api/status/summary',4200);}"
          "function statusScaleHeat(a){var b=[],i;for(i=0;i<a.length;i++)b.push(a[i]/60);return b;}"
          "function statusScaleFilter(a){var b=[],i;for(i=0;i<a.length;i++)b.push(a[i]/3600);return b;}"
          "function statusDrawLineChart(id,data,opt){"
          "opt=opt||{};var c=document.getElementById(id);if(!c||!data||data.length<1)return;"
          "var ctx=c.getContext('2d');if(!ctx)return;"
          "var isTemp=!!opt.temp;var isC=!!opt.celsius;var ySuf=opt.ySuffix||'';"
          "var xL=opt.xLeft||'';var xM=opt.xMid||'';var xR=opt.xRight||'';"
          "function fmtY(v){var s;if(isTemp){s=isC?Number(v).toFixed(1):String(Math.round(Number(v)));}else{s=Number(v).toFixed(2);}"
          "return ySuf?(s+' '+ySuf):s;}"
          "function draw(W,H){"
          "var cc=window.portalChartColors?window.portalChartColors():{bg:'#fff',grid:'#ccc',fg:'#333',gridLine:'#eef1f4'};"
          "ctx.fillStyle=cc.bg;ctx.fillRect(0,0,W,H);ctx.strokeStyle=cc.grid;ctx.strokeRect(0.5,0.5,W-1,H-1);"
          "var lo=Infinity,hi=-Infinity,i,v;"
          "for(i=0;i<data.length;i++){v=Number(data[i]);if(!isFinite(v))continue;if(v<lo)lo=v;if(v>hi)hi=v;}"
          "if(!isFinite(lo)||!isFinite(hi)){ctx.fillStyle=cc.fg;ctx.font='12px sans-serif';ctx.fillText('No data yet',10,H/2);return;}"
          "if(hi-lo<1e-6){lo-=0.5;hi+=0.5;}"
          "var padL=34,padR=8,padT=14,padB=28,pw=W-padL-padR,ph=H-padT-padB;"
          "function yOf(val){return padT+ph-(val-lo)/(hi-lo)*ph;}"
          "ctx.strokeStyle=cc.gridLine;ctx.lineWidth=1;for(i=1;i<=3;i++){var gy=padT+ph*i/4;ctx.beginPath();ctx.moveTo(padL,gy);ctx.lineTo(padL+pw,gy);ctx.stroke();}"
          "ctx.fillStyle=cc.fg;ctx.font='10px sans-serif';ctx.textAlign='left';"
          "ctx.fillText(fmtY(lo),4,H-padB+2);ctx.fillText(fmtY(hi),4,padT+2);"
          "ctx.textAlign='center';if(xL)ctx.fillText(xL,padL,H-4);if(xM)ctx.fillText(xM,padL+pw/2,H-4);if(xR)ctx.fillText(xR,padL+pw,H-4);"
          "ctx.textAlign='left';ctx.strokeStyle='#037e52';ctx.lineWidth=1.5;ctx.beginPath();"
          "for(i=0;i<data.length;i++){var px=padL+i*pw/Math.max(1,data.length-1);var py=yOf(Number(data[i]));if(i===0)ctx.moveTo(px,py);else ctx.lineTo(px,py);}"
          "ctx.stroke();}"
          "function render(){var p=c.parentElement;var pg=document.querySelector('.page');"
          "var cap=(pg&&pg.clientWidth)?pg.clientWidth:((document.documentElement&&document.documentElement.clientWidth)||window.innerWidth||980);"
          "var raw=p?p.clientWidth-2:280;if(raw<0)raw=0;"
          "var W=Math.min(1000,cap,Math.max(260,raw));var H=parseInt(c.getAttribute('height')||'140',10)||140;"
          "H=Math.min(260,H);"
          "var dpr=Math.min(2,Math.max(1,window.devicePixelRatio||1));"
          "c.width=Math.round(W*dpr);c.height=Math.round(H*dpr);c.style.width=W+'px';c.style.height=H+'px';"
          "ctx.setTransform(1,0,0,1,0,0);ctx.scale(dpr,dpr);draw(W,H);}"
          "var raf=0;function scheduleRender(){if(raf)return;raf=window.requestAnimationFrame(function(){raf=0;render();});}"
          "window.addEventListener('resize',scheduleRender,{passive:true});window.addEventListener('orientationchange',scheduleRender,{passive:true});"
          "window.addEventListener('portal-theme-change',scheduleRender,{passive:true});"
          "c.addEventListener('webglcontextlost',function(e){if(e&&e.preventDefault)e.preventDefault();},{passive:false});"
          "c.addEventListener('contextlost',function(e){if(e&&e.preventDefault)e.preventDefault();},{passive:false});"
          "c.addEventListener('contextrestored',scheduleRender,{passive:true});"
          "scheduleRender();}"
          "function statusRenderHistoryCharts(j){"
          "if(!j)return;var tc=!!j.tempIsCelsius;var deg=tc?'\\u00b0C':'\\u00b0F';"
          "statusDrawLineChart('statusTempHistChart',j.tempHistory||[],{temp:1,celsius:tc,ySuffix:deg,xLeft:'-24h',xMid:'-12h',xRight:'now'});"
          "statusDrawLineChart('statusHeatHistChart',statusScaleHeat(j.heatSeconds||[]),{ySuffix:'min',xLeft:'-23d',xMid:'-12d',xRight:'today'});"
          "statusDrawLineChart('statusFilterHistChart',statusScaleFilter(j.filterSeconds||[]),{ySuffix:'hr',xLeft:'-23d',xMid:'-12d',xRight:'today'});}"
          "function statusScrollToHistoryAnchor(id){var el=document.getElementById(id);if(el)el.scrollIntoView({behavior:'smooth',block:'start'});}"
          "function statusSetHistoriesResult(text){var el=document.getElementById('statusHistoriesResult');if(el)el.textContent=text||'';}"
          "function statusHistoriesReady(c){return !!(c&&c.querySelector&&c.querySelector('#statusTempHistSection'));}"
          "let statusHistoriesLoading=false;"
          "async function statusLoadHistories(opt){"
          "opt=opt||{};var scrollTo=opt.scrollTo||'';"
          "var c=document.getElementById('statusHistoriesContainer');var btn=document.getElementById('statusLoadHistoriesBtn');"
          "if(!c){statusSetHistoriesResult('History charts unavailable (page incomplete). Reload /status.');return false;}"
          "if(c.getAttribute('data-loaded')==='1'){statusScrollToHistoryAnchor(scrollTo||'statusTempHistSection');statusSetHistoriesResult('');return true;}"
          "if(statusHistoriesLoading){statusSetHistoriesResult('Still loading history charts...');return false;}"
          "statusHistoriesLoading=true;statusSetHistoriesResult('');"
          "if(btn){btn.disabled=true;btn.textContent='Loading...';}"
          "try{var j=await statusFetchJson('/api/status/histories',9000);"
          "var html=(j&&j.html)?String(j.html):'';"
          "if(!html.length)throw new Error('empty_html');"
          "c.innerHTML=html;"
          "if(!statusHistoriesReady(c)){c.innerHTML='';throw new Error('missing_sections');}"
          "statusRenderHistoryCharts(j);c.setAttribute('data-loaded','1');"
          "if(btn)btn.textContent='History loaded';"
          "statusScrollToHistoryAnchor(scrollTo||'statusTempHistSection');"
          "return true;"
          "}catch(e){c.removeAttribute('data-loaded');"
          "if(btn){btn.disabled=false;btn.textContent='Retry history load';}"
          "statusSetHistoriesResult('History load failed: '+(e&&e.message?e.message:e)+'. Try again.');"
          "return false;"
          "}finally{statusHistoriesLoading=false;}}"
          "function statusEquipCell(key){return document.querySelector('[data-equip=\"'+key+'\"]');}"
          "function statusToggleEquipAbsent(show){var g=document.getElementById('statusEquipGrid');if(!g)return;"
          "if(show){g.classList.remove('status-equip-hide-absent');}else{g.classList.add('status-equip-hide-absent');}}"
          "function statusSetEquipValue(key,text){var c=statusEquipCell(key);if(!c)return;var v=c.querySelector('[data-role=\"value\"]');if(v)v.textContent=text;}"
          "function statusSetEquipStateClass(key,state){var c=statusEquipCell(key);if(!c||c.classList.contains('equip-absent'))return;"
          "var states=['equip-cell--off','equip-cell--low','equip-cell--high','equip-cell--on'];for(var i=0;i<states.length;i++)c.classList.remove(states[i]);"
          "if(state==='off'||state==='low'||state==='high'||state==='on')c.classList.add('equip-cell--'+state);}"
          "function statusPumpVisualState(snap,num){var cfg=Number(snap['pump'+num+'Config']||0);if(cfg===1)return snap['pump'+num+'On']?'on':'off';"
          "var raw=Number(snap['pump'+num]||0);if(raw===0)return 'off';if(raw===1)return 'low';return 'high';}"
          "function statusBinaryEquipFromSnap(snap,key){if(typeof snap[key]==='undefined')return 'off';return Number(snap[key])>0?'on':'off';}"
          "function statusSetButtonState(code,desired){var btn=document.querySelector('button[data-button=\"'+code+'\"]');if(!btn)return;"
          "btn.setAttribute('data-state',desired);btn.textContent='Turn '+(desired==='on'?'On':'Off');}"
          "function statusPumpDisplay(raw){if(raw===0)return 'Off';if(raw===1)return 'Low';if(raw===2)return 'High';return String(raw);}"
          "function statusOnOff(v){return Number(v)>0?'On':'Off';}"
          "function statusPumpUiValue(snap,num){var cfg=Number(snap['pump'+num+'Config']||0);if(cfg===1)return statusOnOff(snap['pump'+num+'On']);return statusPumpDisplay(Number(snap['pump'+num]||0));}"
          "function statusFormatBandTemp(snap,v){var n=Number(v);if(!isFinite(n)||n<=0)return'\xe2\x80\x94';var c=!!snap.tempScaleCelsius;"
          "if(c)return n.toFixed(1)+'\\u00b0C';return String(Math.round(n))+'\\u00b0F';}"
          "function statusApplySnapshotMeta(snap){var el=document.getElementById('statusSnapshotMeta');if(!el||!snap)return;"
          "if(typeof snap.snapshotMeta==='string'){el.textContent=snap.snapshotMeta;"
          "if(typeof snap.snapshotAtLocal==='string'&&snap.snapshotAtLocal.length)el.title=snap.snapshotAtLocal;}}"
          "function statusApplyHeatingSnap(snap){"
          "if(typeof snap.spaStateText==='undefined')return;"
          "var hero=document.getElementById('statusSpaStateHero');if(hero)hero.textContent=snap.spaStateText;"
          "var heroEl=document.getElementById('statusSpaHero');"
          "if(heroEl){heroEl.className='heat-hero';var im=Number(snap.initMode||0);var ss=Number(snap.spaState||0);"
          "if(im===2)heroEl.classList.add('heat-hero--alert');else if(ss===1||im===1)heroEl.classList.add('heat-hero--init');else heroEl.classList.add('heat-hero--ok');}"
          "var hsn=Number(snap.heatingState||0);var hth=document.getElementById('statusHeatStateHero');if(hth)hth.textContent=snap.heatingStateText||'';"
          "var hhero=document.getElementById('statusHeatHero');if(hhero){hhero.className='heat-hero';"
          "if(hsn===1)hhero.classList.add('heat-hero--heat-on');else if(hsn===2)hhero.classList.add('heat-hero--heat-alt');else if(hsn===3)hhero.classList.add('heat-hero--heat-reserved');else hhero.classList.add('heat-hero--heat-idle');}"
          "var imv=document.getElementById('statusInitModeVal');if(imv)imv.textContent=snap.initModeText||'';"
          "var hmv=document.getElementById('statusHeatingModeVal');if(hmv)hmv.textContent=snap.heatingModeText||'';"
          "var raw=document.getElementById('statusHeatRawPre');if(raw)raw.textContent='spaState='+snap.spaState+' initMode='+snap.initMode+' heatingMode='+snap.heatingMode+' heatingState='+snap.heatingState+' needsHeat='+snap.needsHeat;"
          "}"
          "function statusApplySnapshot(snap){"
          "if(!snap)return;"
          "statusSetEquipValue('pump1',statusPumpUiValue(snap,1));statusSetEquipStateClass('pump1',statusPumpVisualState(snap,1));"
          "statusSetEquipValue('pump2',statusPumpUiValue(snap,2));statusSetEquipStateClass('pump2',statusPumpVisualState(snap,2));"
          "statusSetEquipValue('pump3',statusPumpUiValue(snap,3));statusSetEquipStateClass('pump3',statusPumpVisualState(snap,3));"
          "statusSetEquipValue('pump4',statusPumpUiValue(snap,4));statusSetEquipStateClass('pump4',statusPumpVisualState(snap,4));"
          "statusSetEquipValue('pump5',statusPumpUiValue(snap,5));statusSetEquipStateClass('pump5',statusPumpVisualState(snap,5));"
          "statusSetEquipValue('pump6',statusPumpUiValue(snap,6));statusSetEquipStateClass('pump6',statusPumpVisualState(snap,6));"
          "if(typeof snap.circ!=='undefined'){statusSetEquipValue('circ',statusOnOff(snap.circ));statusSetEquipStateClass('circ',statusBinaryEquipFromSnap(snap,'circ'));}"
          "statusSetEquipValue('blower',statusOnOff(snap.blower));statusSetEquipStateClass('blower',statusBinaryEquipFromSnap(snap,'blower'));"
          "statusSetEquipValue('light1',statusOnOff(snap.light1));statusSetEquipStateClass('light1',statusBinaryEquipFromSnap(snap,'light1'));"
          "statusSetEquipValue('light2',statusOnOff(snap.light2));statusSetEquipStateClass('light2',statusBinaryEquipFromSnap(snap,'light2'));"
          "statusSetEquipValue('mister',statusOnOff(snap.mister));statusSetEquipStateClass('mister',statusBinaryEquipFromSnap(snap,'mister'));"
          "statusSetButtonState(4,snap.pump1On?'off':'on');"
          "statusSetButtonState(5,snap.pump2On?'off':'on');"
          "statusSetButtonState(6,snap.pump3On?'off':'on');"
          "statusSetButtonState(7,snap.pump4On?'off':'on');"
          "statusSetButtonState(8,snap.pump5On?'off':'on');"
          "statusSetButtonState(9,snap.pump6On?'off':'on');"
          "statusSetButtonState(12,Number(snap.blower)>0?'off':'on');"
          "statusSetButtonState(17,Number(snap.light1)>0?'off':'on');"
          "statusSetButtonState(18,Number(snap.light2)>0?'off':'on');"
          "statusSetButtonState(14,Number(snap.mister)>0?'off':'on');"
          "var tr=Number(snap.tempRange||0);var hi=document.getElementById('statusBandHigh');var lo=document.getElementById('statusBandLow');"
          "if(hi){hi.classList.toggle('range-band-active-high',tr===1);hi.classList.remove('range-band-active-low');}"
          "if(lo){lo.classList.toggle('range-band-active-low',tr===0);lo.classList.remove('range-band-active-high');}"
          "var hv=document.getElementById('statusBandHighVal');var lv=document.getElementById('statusBandLowVal');"
          "if(hv)hv.textContent=statusFormatBandTemp(snap,snap.highSetTemp);if(lv)lv.textContent=statusFormatBandTemp(snap,snap.lowSetTemp);"
          "var lbl=document.getElementById('statusSetTempScopeLabel');if(lbl)lbl.textContent=tr===1?'(high range)':'(low range)';"
          "if(lo){lo.disabled=(tr===0);lo.setAttribute('aria-pressed',tr===0?'true':'false');}"
          "if(hi){hi.disabled=(tr===1);hi.setAttribute('aria-pressed',tr===1?'true':'false');}"
          "var setInput=document.getElementById('statusSetTempInput');"
          "if(setInput){if(typeof snap.setTempMin!=='undefined')setInput.min=String(snap.setTempMin);if(typeof snap.setTempMax!=='undefined')setInput.max=String(snap.setTempMax);"
          "if(typeof snap.tempScaleCelsius!=='undefined')setInput.step=snap.tempScaleCelsius?'0.5':'1';"
          "if(document.activeElement!==setInput&&typeof snap.setTemp!=='undefined')setInput.value=String(snap.tempScaleCelsius?Number(snap.setTemp).toFixed(1):Math.round(Number(snap.setTemp)));}"
          "var uC=!!snap.tempScaleCelsius;var cBtn=document.getElementById('statusTempUnitsToggleC');var fBtn=document.getElementById('statusTempUnitsToggleF');"
          "if(cBtn)cBtn.disabled=uC;if(fBtn)fBtn.disabled=!uC;"
          "var pt=document.getElementById('statusPanelTimeVal');if(pt&&typeof snap.panelTime==='string')pt.textContent=snap.panelTime;"
          "var cf=document.getElementById('statusClockFormatVal');if(cf&&typeof snap.clockFormat==='string'){cf.textContent=snap.clockFormat;if(typeof snap.clockModeRaw!=='undefined')cf.title='Raw status flag (status byte 9 & 0x02): '+snap.clockModeRaw;}"
          "var f12=document.getElementById('statusClockFormat12Btn');var f24=document.getElementById('statusClockFormat24Btn');var is24=String(snap.clockFormat||'').toLowerCase().indexOf('24')>=0;"
          "if(f12)f12.disabled=!is24;if(f24)f24.disabled=is24;"
          "var fm=document.getElementById('statusFilterModeVal');if(fm&&typeof snap.filterModeText==='string')fm.textContent=snap.filterModeText;"
          "function statusApplyReminderSnap(snap){"
          "var txt=typeof snap.reminderText==='string'?snap.reminderText:'';"
          "var active=!!(snap.reminderActive||(txt&&txt!=='None'));"
          "var fault=!!snap.reminderIsFault;"
          "var banner=document.getElementById('statusReminderBanner');"
          "if(banner){banner.classList.toggle('is-active',active);"
          "banner.classList.remove('status-reminder-banner--warn','status-reminder-banner--fault');"
          "if(active)banner.classList.add(fault?'status-reminder-banner--fault':'status-reminder-banner--warn');"
          "var bv=document.getElementById('statusReminderBannerVal');if(bv)bv.textContent=active?txt:'';"
          "var bh=document.getElementById('statusReminderBannerHint');"
          "if(bh)bh.textContent=active&&(typeof snap.reminderHint==='string')?snap.reminderHint:'';}"
          "var rv=document.getElementById('statusReminderVal');if(rv&&typeof snap.reminderText==='string')rv.textContent=snap.reminderText;"
          "var rr=document.getElementById('statusReminderRow');if(rr){rr.classList.remove('kv-row--alert','kv-row--alert-warn');"
          "if(active)rr.classList.add(fault?'kv-row--alert':'kv-row--alert-warn');}}"
          "statusApplyReminderSnap(snap);"
          "var tIn=document.getElementById('statusPanelTimeInput');if(tIn&&document.activeElement!==tIn&&typeof snap.panelTime==='string')tIn.value=snap.panelTime;"
          "statusApplyHeatingSnap(snap);statusApplySnapshotMeta(snap);"
          "}"
          "let statusPollTimer=0;let statusPollBusy=false;let statusPollFailures=0;let statusLastSnapshotAgeSec=0;const statusPollBaseMs=2000;const statusPollMaxMs=25000;const statusFlakyFailThreshold=3;const statusStaleAgeSecThreshold=10;"
          "function statusConnState(msg){var el=document.getElementById('statusButtonResult');if(el&&msg)el.textContent=msg;}"
          "function statusShouldShowFlaky(){if(statusPollFailures<statusFlakyFailThreshold)return false;if(statusLastSnapshotAgeSec===0)return true;return statusLastSnapshotAgeSec>=statusStaleAgeSecThreshold;}"
          "function statusSchedulePoll(ms){statusStopPolling();statusPollTimer=setTimeout(statusPollOnce,Math.max(250,ms||statusPollBaseMs));}"
          "async function statusPollOnce(){"
          "if(statusPollBusy||document.hidden)return;"
          "statusPollBusy=true;"
          "try{var snap=await statusFetchControls();statusApplySnapshot(snap);statusLastSnapshotAgeSec=Number(snap&&snap.snapshotAgeSec||0);statusPollFailures=0;statusConnState('');statusSchedulePoll(statusPollBaseMs);}catch(e){statusPollFailures++;if(statusShouldShowFlaky())statusConnState('Connection is flaky, showing last known values...');statusSchedulePoll(statusBackoffMs(statusPollBaseMs,statusPollMaxMs,statusPollFailures));}"
          "statusPollBusy=false;"
          "}"
          "function statusStartPolling(){if(statusPollTimer||statusPollBusy)return;statusPollFailures=0;statusPollOnce();}"
          "function statusStopPolling(){if(!statusPollTimer)return;clearTimeout(statusPollTimer);statusPollTimer=0;}"
          "document.addEventListener('visibilitychange',function(){if(document.hidden){statusStopPolling();}else{statusStartPolling();}});"
          "window.addEventListener('beforeunload',statusStopPolling);"
          "(function(){"
          "var loadBtn=document.getElementById('statusLoadHistoriesBtn');"
          "if(loadBtn)loadBtn.addEventListener('click',function(){statusLoadHistories({});});"
          "document.addEventListener('click',function(e){"
          "var t=e.target&&e.target.closest?e.target.closest('a.status-temp-chart-link[data-history-anchor]'):null;"
          "if(!t)return;var anchor=t.getAttribute('data-history-anchor');if(!anchor)return;"
          "e.preventDefault();statusLoadHistories({scrollTo:anchor});});"
          "statusStartPolling();"
          "})();"
          "function statusButtonMatch(snap,code,desired){var on=(desired||'on').toLowerCase()==='on';"
          "if(code===17)return (snap.light1>0)===on;"
          "if(code===18)return (snap.light2>0)===on;"
          "if(code===4)return on?!!snap.pump1On:!snap.pump1On;"
          "if(code===5)return on?!!snap.pump2On:!snap.pump2On;"
          "if(code===6)return on?!!snap.pump3On:!snap.pump3On;"
          "if(code===7)return on?!!snap.pump4On:!snap.pump4On;"
          "if(code===8)return on?!!snap.pump5On:!snap.pump5On;"
          "if(code===9)return on?!!snap.pump6On:!snap.pump6On;"
          "if(code===12)return on?(snap.blower>0):(snap.blower===0);"
          "if(code===14)return (snap.mister>0)===on;"
          "if(code===80)return on?(Number(snap.tempRange||0)===1):(Number(snap.tempRange||0)===0);"
          "return false;}"
          "async function statusWaitForButtonState(code,desired){"
          "for(var i=0;i<10;i++){await new Promise(function(res){setTimeout(res,650);});"
          "try{var snap=await statusFetchControls();if(statusButtonMatch(snap,code,desired))return true;}catch(e){}}"
          "return false;}"
          "async function statusWaitForSetTemp(target){"
          "for(var i=0;i<10;i++){await new Promise(function(res){setTimeout(res,650);});"
          "try{var snap=await statusFetchControls();if(Math.abs(Number(snap.setTemp)-Number(target))<0.26)return true;}catch(e){}}"
          "return false;}"
          "async function statusWaitForTempUnits(units){"
          "var wantC=(String(units||'').toUpperCase()==='C');"
          "for(var i=0;i<10;i++){await new Promise(function(res){setTimeout(res,650);});"
          "try{var snap=await statusFetchControls();if(!!snap.tempScaleCelsius===wantC)return true;}catch(e){}}"
          "return false;}"
          "function statusSetResult(id,text){var el=document.getElementById(id);if(el)el.textContent=text;}"
          "async function statusSendButton(btn){"
          "try{btn.disabled=true;const c=btn.getAttribute('data-button');const s=btn.getAttribute('data-state')||'on';"
          "const xml='<device_request target_name=\"Button\">'+c+':'+s+'</device_request>';"
          "const out=await statusSendSci(xml);if(out.indexOf('result=\\'accepted\\'')<0){statusSetResult('statusButtonResult','Button command response: '+out);return;}"
          "statusSetResult('statusButtonResult','Button command accepted; waiting for spa status update...');"
          "const changed=await statusWaitForButtonState(Number(c),s);"
          "if(changed){statusSetResult('statusButtonResult','Button command accepted and state changed.');setTimeout(function(){location.reload();},500);}else{statusSetResult('statusButtonResult','Button command accepted, but state did not change yet.');}"
          "}catch(e){statusSetResult('statusButtonResult','Button command failed: '+e);}finally{btn.disabled=false;}"
          "}"
          "async function statusSendSetTemp(){"
          "const input=document.getElementById('statusSetTempInput');if(!input)return;const v=input.value;"
          "var pv=parseFloat(v);var mn=parseFloat(input.min);var mx=parseFloat(input.max);"
          "if(isFinite(pv)&&isFinite(mn)&&isFinite(mx)&&(pv<mn-1e-6||pv>mx+1e-6)){statusSetResult('statusSetTempResult','Enter a value between '+mn+' and '+mx+' for the active range.');return;}"
          "try{const xml='<device_request target_name=\"SetTemp\">'+v+'</device_request>';"
          "const out=await statusSendSci(xml);if(out.indexOf('result=\\'accepted\\'')<0){statusSetResult('statusSetTempResult','SetTemp response: '+out);return;}"
          "statusSetResult('statusSetTempResult','SetTemp accepted; waiting for spa status update...');"
          "const changed=await statusWaitForSetTemp(v);"
          "if(changed){statusSetResult('statusSetTempResult','SetTemp accepted and state changed.');setTimeout(function(){location.reload();},500);}else{statusSetResult('statusSetTempResult','SetTemp accepted, but setpoint did not change yet.');}"
          "}catch(e){statusSetResult('statusSetTempResult','SetTemp failed: '+e);}"
          "}"
          "async function statusSendTempUnits(units){"
          "var t=String(units||'').toUpperCase();if(t!=='C'&&t!=='F'){statusSetResult('statusTempUnitsResult','Invalid temp units request.');return;}"
          "if(!confirm('Change temperature units to '+(t==='C'?'Celsius':'Fahrenheit')+'?')){statusSetResult('statusTempUnitsResult','Temperature units change canceled.');return;}"
          "var cBtn=document.getElementById('statusTempUnitsToggleC');var fBtn=document.getElementById('statusTempUnitsToggleF');"
          "try{if(cBtn)cBtn.disabled=true;if(fBtn)fBtn.disabled=true;"
          "const xml='<device_request target_name=\"TempUnits\">'+t+'</device_request>';"
          "const out=await statusSendSci(xml);if(out.indexOf('result=\\'accepted\\'')<0){statusSetResult('statusTempUnitsResult','TempUnits response: '+out);return;}"
          "statusSetResult('statusTempUnitsResult','TempUnits accepted; waiting for spa status update...');"
          "const changed=await statusWaitForTempUnits(t);"
          "if(changed){statusSetResult('statusTempUnitsResult','Temperature units updated.');statusApplySnapshot(await statusFetchControls());}"
          "else{statusSetResult('statusTempUnitsResult','Command accepted; temperature units did not update yet.');}}"
          "catch(e){statusSetResult('statusTempUnitsResult','TempUnits failed: '+e);}finally{"
          "try{var snap=await statusFetchControls();statusApplySnapshot(snap);}catch(_e){}"
          "if(cBtn)cBtn.disabled=false;if(fBtn)fBtn.disabled=false;}"
          "}"
          "async function statusWaitForPanelTime(target){"
          "for(var i=0;i<10;i++){await new Promise(function(res){setTimeout(res,650);});"
          "try{var snap=await statusFetchControls();if(String(snap.panelTime||'')===String(target))return true;}catch(e){}}"
          "return false;}"
          "async function statusWaitForTimeFormat(use24){"
          "for(var i=0;i<10;i++){await new Promise(function(res){setTimeout(res,650);});"
          "try{var snap=await statusFetchControls();var is24=String(snap.clockFormat||'').toLowerCase().indexOf('24')>=0;if(is24===!!use24)return true;}catch(e){}}"
          "return false;}"
          "async function statusSendTimeFormat(fmt){"
          "var f=Number(fmt);if(f!==12&&f!==24){statusSetResult('statusTimeFormatResult','Invalid time format request.');return;}"
          "if(!confirm('Change panel clock format to '+f+'-hour?')){statusSetResult('statusTimeFormatResult','Time format change canceled.');return;}"
          "var f12=document.getElementById('statusClockFormat12Btn');var f24=document.getElementById('statusClockFormat24Btn');"
          "try{if(f12)f12.disabled=true;if(f24)f24.disabled=true;"
          "const xml='<device_request target_name=\"TimeFormat\">'+String(f)+'</device_request>';"
          "const out=await statusSendSci(xml);if(out.indexOf('result=\\'accepted\\'')<0){statusSetResult('statusTimeFormatResult','TimeFormat response: '+out);return;}"
          "statusSetResult('statusTimeFormatResult','TimeFormat accepted; waiting for spa...');"
          "const changed=await statusWaitForTimeFormat(f===24);"
          "if(changed){statusSetResult('statusTimeFormatResult','Panel clock format updated.');statusApplySnapshot(await statusFetchControls());}"
          "else{statusSetResult('statusTimeFormatResult','Command accepted; panel clock format did not update yet.');}"
          "}catch(e){statusSetResult('statusTimeFormatResult','TimeFormat failed: '+e);}finally{"
          "try{var snap=await statusFetchControls();statusApplySnapshot(snap);}catch(_e){}"
          "if(f12)f12.disabled=false;if(f24)f24.disabled=false;}"
          "}"
          "async function statusSendPanelTime(){"
          "var input=document.getElementById('statusPanelTimeInput');if(!input)return;var v=(input.value||'').trim();"
          "if(!/^\\d{1,2}:\\d{2}$/.test(v)){statusSetResult('statusSystemTimeResult','Enter a valid time (HH:MM).');return;}"
          "var p=v.split(':');var hh=String(Number(p[0])||0);var mm=String(p[1]||'00');if(mm.length===1)mm='0'+mm;if(hh.length===1)hh='0'+hh;v=hh+':'+mm;"
          "try{const xml='<device_request target_name=\"SystemTime\">'+v+'</device_request>';"
          "const out=await statusSendSci(xml);if(out.indexOf('result=\\'accepted\\'')<0){statusSetResult('statusSystemTimeResult','SystemTime response: '+out);return;}"
          "statusSetResult('statusSystemTimeResult','SystemTime accepted; waiting for spa...');"
          "const changed=await statusWaitForPanelTime(v);"
          "if(changed){statusSetResult('statusSystemTimeResult','Panel time updated.');statusApplySnapshot(await statusFetchControls());}"
          "else{statusSetResult('statusSystemTimeResult','Command accepted; panel time did not match yet.');}"
          "}catch(e){statusSetResult('statusSystemTimeResult','SystemTime failed: '+e);}"
          "}"
          "async function statusSyncPanelTimeFromGateway(){"
          "try{var snap=await statusFetchControls();var gt=snap.gatewayTimeHHMM;if(!gt||gt==='--:--'){statusSetResult('statusSystemTimeResult','Gateway clock not available (sync ESP time / NTP).');return;}"
          "const xml='<device_request target_name=\"SystemTime\">'+gt+'</device_request>';"
          "const out=await statusSendSci(xml);if(out.indexOf('result=\\'accepted\\'')<0){statusSetResult('statusSystemTimeResult','SystemTime response: '+out);return;}"
          "statusSetResult('statusSystemTimeResult','Sync sent; waiting for spa...');"
          "const changed=await statusWaitForPanelTime(gt);"
          "if(changed){statusSetResult('statusSystemTimeResult','Panel time synced from gateway.');var inp=document.getElementById('statusPanelTimeInput');if(inp)inp.value=gt;statusApplySnapshot(await statusFetchControls());}"
          "else{statusSetResult('statusSystemTimeResult','Command accepted; panel time did not match yet.');}"
          "}catch(e){statusSetResult('statusSystemTimeResult','Sync failed: '+e);}"
          "}"
          "</script>";
  html += "</div></main></div></body></html>";
  String etag = String("W/\"status-") + String(VERSION) + "-" + String(BUILD) + "-" + String(spaStatusData.lastUpdate) + "-" + String(spaConfigurationData.lastUpdate) + "\"";
  logPortalHtmlMissingGlobalCss(sawPortalCss, html.length(), "/status");
  const size_t statusOutLen = html.length();
  if (!wroteOpeningTag)
  {
    Log.error("[Web]: /status assemble missing <html> prefix len=%u from %p" CR,
              static_cast<unsigned>(statusOutLen), request->client()->remoteIP());
  }
  Log.verbose(F("[Web]: /status assembled len=%u slabs=%u freeHeap=%u maxAlloc=%u healthy=%u" CR),
              static_cast<unsigned>(statusOutLen), static_cast<unsigned>(html.slabCount()),
              static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()),
              html.healthy() ? 1u : 0u);
  sendHtmlChunksWithEtag(request, html, etag);
  // Never log full `html` here: /status payload is ~40KB+; printf-style verbose would blow stack/heap.
  Log.verbose(F("[Web]: /status sent len=%u" CR), static_cast<unsigned>(statusOutLen));
}

static String spaConfigTrimCopy(const char *s)
{
  String out(s ? s : "");
  out.trim();
  return out;
}

/** Balboa Information Response inner payload starts at rawData[5] (see parseInformationResponse). */
static String spaInformationConfigSignatureHex(const SpaInformationData &inf)
{
  constexpr unsigned kNeed = 5 + 17;
  if (inf.rawDataLength < kNeed)
  {
    return String("(pending)");
  }
  const uint8_t *p = inf.rawData + 5;
  char buf[16];
  snprintf(buf, sizeof(buf), "%02X%02X%02X%02X", static_cast<unsigned>(p[13]), static_cast<unsigned>(p[14]),
           static_cast<unsigned>(p[15]), static_cast<unsigned>(p[16]));
  return String(buf);
}

static String spaInformationHeaterVoltageLabel(uint8_t hv)
{
  char hx[16];
  snprintf(hx, sizeof(hx), "0x%02X", static_cast<unsigned>(hv));
  if (hv == 0x01)
  {
    return String("240V class (") + hx + ")";
  }
  return String(hx);
}

static String spaInformationHeaterTypeLabel(uint8_t ht)
{
  char hx[16];
  snprintf(hx, sizeof(hx), "0x%02X", static_cast<unsigned>(ht));
  if (ht == 0x0a || ht == 0x06)
  {
    return String("Standard (") + hx + ")";
  }
  return String(hx);
}

static String spaHexWordsUpper(const uint8_t *data, uint8_t len, size_t maxShow)
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

static String configTimeInputValue(uint8_t hour, uint8_t minute)
{
  char buf[8];
  snprintf(buf, sizeof(buf), "%02u:%02u", static_cast<unsigned>(hour), static_cast<unsigned>(minute));
  return String(buf);
}

/** Panel clock display: status byte 9 bit 1 (0x02) = 24-hour; clear = 12-hour AM/PM. */
static bool spaPanelClockFormatIs24Hour()
{
  if (!spaHasFreshStatus())
  {
    return true;
  }
  return (spaStatusData.clockMode & 0x02) != 0;
}

/** `type=time` values stay HH:MM (24h wire format); `lang` steers the native picker UI. */
static String configFilterTimeInputLangAttr(bool use24Hour)
{
  return String(" lang=\"") + (use24Hour ? "en-GB" : "en-US") + "\"";
}

static String configFilterDurationFieldsHtml(const char *hourId, const char *minId, uint8_t durHour, uint8_t durMinute)
{
  return String("<span class=\"config-dur-fields\">") + "<input id='" + hourId +
         "' type='number' min='0' max='24' step='1' value='" + String(durHour) + "' aria-label='Duration hours' />" +
         "<span class=\"config-dur-unit\">h</span>" + "<input id='" + minId +
         "' type='number' min='0' max='59' step='15' value='" + String(durMinute) + "' aria-label='Duration minutes' />" +
         "<span class=\"config-dur-unit\">min</span></span>";
}

static const char *configFaultSeverityClass(const char *severity)
{
  if (severity != nullptr && String(severity) == "alert")
  {
    return "config-fault-sev--alert";
  }
  if (severity != nullptr && String(severity) == "warning")
  {
    return "config-fault-sev--warning";
  }
  return "config-fault-sev--info";
}

template <typename HtmlOut>
static void appendConfigFaultSeverityBadge(HtmlOut &html, uint8_t faultCode)
{
  const char *severity = spaFaultLogSeverityText(faultCode);
  html += "<span class=\"config-fault-sev ";
  html += configFaultSeverityClass(severity);
  html += "\">";
  html += severity;
  html += "</span>";
}

void handleConfig(AsyncWebServerRequest *request)
{
  if (!portalLargePageTryBegin(request, "/config"))
  {
    return;
  }
  // Log.verbose("[Web]: Request %s received from %p" CR, request->url().c_str(), request->client()->remoteIP());

  const char *configEnhancements =
      "<style>"
      "html{scroll-behavior:smooth;}"
      ".config-layout{display:grid;grid-template-columns:1fr;gap:var(--space-3);}"
      "@media (min-width:720px){.config-layout{grid-template-columns:1fr 1fr;}}"
      ".config-layout .panel{margin-bottom:0;}"
      ".config-span-full{grid-column:1/-1;}"
      ".config-toc{display:flex;flex-wrap:wrap;gap:8px 14px;align-items:center;margin:0 0 var(--space-2) 0;padding:0;list-style:none;}"
      ".config-toc li{margin:0;padding:0;border-bottom:none !important;}"
      ".config-toc a{font-size:14px;color:var(--heading);text-decoration:underline;}"
      ".config-toc a:focus-visible{outline:2px solid var(--focus);outline-offset:2px;border-radius:2px;}"
      "dl.config-kv{margin:0;padding:0;}"
      "dl.config-kv .kv-row{display:grid;grid-template-columns:minmax(110px,42%) 1fr;gap:6px 12px;padding:var(--space-1) 0;"
      "border-bottom:1px dashed var(--border);align-items:start;}"
      "dl.config-kv .kv-row:last-child{border-bottom:none;}"
      "dl.config-kv dt{margin:0;font-weight:600;color:var(--muted);font-size:0.92rem;}"
      "dl.config-kv dd{margin:0;overflow-wrap:anywhere;word-break:break-word;}"
      "table.config-equip{width:100%;border-collapse:collapse;margin-top:8px;}"
      "table.config-equip th,table.config-equip td{padding:8px;border-bottom:1px solid var(--border);text-align:left;vertical-align:top;}"
      "table.config-equip th{font-size:13px;color:var(--muted);}"
      "table.config-equip tr.config-equip-absent td{color:var(--muted);opacity:0.78;}"
      "table.config-equip tr.config-equip-absent td:first-child{font-weight:500;}"
      ".config-filter-strip{display:grid;grid-template-columns:1fr 1fr;gap:12px;align-items:stretch;margin:0 0 var(--space-3) 0;}"
      "@media (max-width:560px){.config-filter-strip{grid-template-columns:1fr;}}"
      ".config-filter-card{display:flex;flex-direction:column;border:1px solid var(--border);border-radius:10px;padding:10px 12px;background:var(--surface-2);}"
      ".config-filter-strip .config-filter-card{min-height:100%;}"
      ".config-filter-card h2{margin:0 0 8px 0;font-size:0.88rem;color:var(--muted);font-weight:700;text-transform:uppercase;letter-spacing:.02em;}"
      ".config-filter-card p{margin:4px 0;font-size:14px;}"
      ".config-filter-card label{display:block;font-size:13px;color:var(--muted);margin:8px 0 4px 0;}"
      ".config-filter-card label.config-filter-enable{margin-top:0;}"
      ".config-filter-fields{margin-top:auto;}"
      ".config-filter-card input[type=time]{width:100%;max-width:160px;padding:6px 8px;font-size:14px;}"
      ".config-dur-fields{display:flex;flex-wrap:wrap;align-items:center;gap:6px 8px;}"
      ".config-dur-fields input[type=number]{width:4.5em;padding:6px 8px;font-size:14px;}"
      ".config-dur-unit{font-size:14px;color:var(--muted);}"
      ".config-backup-actions{display:flex;flex-wrap:wrap;gap:10px;align-items:center;margin:10px 0;}"
      "#cfgImportPreview{margin:10px 0;font-size:14px;white-space:pre-wrap;}"
      ".config-fault-latest{margin:0 0 12px 0;padding:12px;border:1px solid var(--border);border-radius:8px;background:var(--surface-2);}"
      ".config-fault-latest h2{margin:0 0 8px 0;font-size:1rem;}"
      ".config-fault-headline{font-size:18px;font-weight:700;margin:0 0 8px 0;line-height:1.35;}"
      ".config-fault-meta{margin:4px 0;font-size:14px;color:var(--muted);}"
      ".config-fault-sev{display:inline-block;font-size:11px;font-weight:700;text-transform:uppercase;padding:2px 8px;border-radius:999px;margin-left:8px;vertical-align:middle;}"
      ".config-fault-sev--info{color:#0f5132;background:#d1e7dd;}"
      ".config-fault-sev--warning{color:#664d03;background:#fff3cd;}"
      ".config-fault-sev--alert{color:#842029;background:#f8d7da;}"
      "table.config-fault-history{width:100%;border-collapse:collapse;margin-top:8px;font-size:14px;}"
      "table.config-fault-history th,table.config-fault-history td{padding:8px;border-bottom:1px solid var(--border);text-align:left;vertical-align:top;}"
      "table.config-fault-history th{font-size:13px;color:var(--muted);}"
      "pre.config-hex{margin:8px 0 0 0;padding:10px 12px;font-size:12px;line-height:1.45;font-family:ui-monospace,Menlo,Consolas,monospace;"
      "overflow-x:auto;word-break:break-all;white-space:pre-wrap;background:var(--surface-2);border:1px solid var(--border);border-radius:8px;}"
      ".config-layout details{margin-top:10px;}"
      "</style>";

  PortalHtmlChunks html;
  html = F("<html>");
  const bool sawPortalCss = appendPortalHead(html, "Spa Config");
  html += configEnhancements;
  html += F("<body><a class='skip-link' href='#mainContent'>Skip to main content</a><div class='page'>");
  html += webMenuConfig;
  html += F("<main id='mainContent'>");
  html += ePaper;

  html += "<nav aria-label='Spa Config sections'><ul class='config-toc'>"
          "<li><a href='#cfg-backup'>Backup &amp; restore</a></li>"
          "<li><a href='#cfg-equipment'>Equipment wiring</a></li>"
          "<li><a href='#cfg-identity'>Controller identity</a></li>"
          "<li><a href='#cfg-filter'>Filter configuration</a></li>"
          "<li><a href='#cfg-preferences'>Panel preferences</a></li>"
          "<li><a href='#cfg-history'>Spa controller history</a></li>"
          "<li><a href='#cfg-littlefs'>LittleFS</a></li>"
          "</ul></nav>";

  html += "<div class='config-layout'>";

  html += "<section class='panel config-span-full' id='cfg-backup'><h1>Backup &amp; restore</h1>";
  html += "<p class=\"chart-caption\" style=\"margin:0 0 10px 0\">Download writable spa settings (filter schedules, panel clock, temp units) plus read-only snapshots for identity checks. "
          "Restore applies writable sections only; equipment wiring and preferences snapshots are never written to the controller.</p>";
  html += "<div class=\"config-backup-actions\">"
          "<button class='equip-btn' type='button' id='cfgExportBtn'>Download config</button>"
          "<input type='file' id='cfgImportFile' accept='application/json,.json' />"
          "<button class='equip-btn' type='button' id='cfgImportPreviewBtn'>Preview restore</button>"
          "<button class='equip-btn' type='button' id='cfgImportApplyBtn'>Apply restore</button>"
          "<label style='font-size:14px'><input type='checkbox' id='cfgImportForce'/> Force (ignore spa identity mismatch)</label>"
          "</div>";
  html += "<pre id='cfgImportPreview' class='config-hex' style='display:none'></pre></section>";

  html += "<section class='panel' id='cfg-equipment'><h1>Equipment wiring (configuration)</h1>";
  html += "<p class=\"chart-caption\" style=\"margin:0 0 10px 0\">For live state and controls, see <a href='/status'>Spa Status</a>.</p>";
  if (spaConfigurationData.lastUpdate == 0)
  {
    html += "<p style=\"margin:0\"><em>Configuration frame not available yet.</em></p>";
  }
  else
  {
    html += "<dl class=\"config-kv\">";
    html += "<div class=\"kv-row\"><dt>lastUpdate</dt><dd>" + statusLastUpdateDisplayHtml(spaConfigurationData.lastUpdate) + "</dd></div>";
    html += "</dl>";
    html += "<table class=\"config-equip\"><thead><tr><th>Load</th><th>Configured as</th></tr></thead><tbody>";
    appendConfigEquipRow(html, "Pump 1", spaConfigPumpInstallLabel(spaConfigurationData.pump1), spaConfigurationData.pump1 == 0);
    appendConfigEquipRow(html, "Pump 2", spaConfigPumpInstallLabel(spaConfigurationData.pump2), spaConfigurationData.pump2 == 0);
    appendConfigEquipRow(html, "Pump 3", spaConfigPumpInstallLabel(spaConfigurationData.pump3), spaConfigurationData.pump3 == 0);
    appendConfigEquipRow(html, "Pump 4", spaConfigPumpInstallLabel(spaConfigurationData.pump4), spaConfigurationData.pump4 == 0);
    appendConfigEquipRow(html, "Pump 5", spaConfigPumpInstallLabel(spaConfigurationData.pump5), spaConfigurationData.pump5 == 0);
    appendConfigEquipRow(html, "Pump 6", spaConfigPumpInstallLabel(spaConfigurationData.pump6), spaConfigurationData.pump6 == 0);
    appendConfigEquipRow(html, "Light 1", spaConfigLightInstallLabel(spaConfigurationData.light1), spaConfigurationData.light1 == 0);
    appendConfigEquipRow(html, "Light 2", spaConfigLightInstallLabel(spaConfigurationData.light2), spaConfigurationData.light2 == 0);
    appendConfigEquipRow(html, "Blower", spaConfigLoadInstallLabel(spaConfigurationData.blower), !spaConfigurationData.blower);
    appendConfigEquipRow(html, "Circulation pump", spaConfigLoadInstallLabel(spaConfigurationData.circulationPump),
                         !spaConfigurationData.circulationPump);
    appendConfigEquipRow(html, "Aux 1", spaConfigLoadInstallLabel(spaConfigurationData.aux1), !spaConfigurationData.aux1);
    appendConfigEquipRow(html, "Aux 2", spaConfigLoadInstallLabel(spaConfigurationData.aux2), !spaConfigurationData.aux2);
    appendConfigEquipRow(html, "Mister", spaConfigLoadInstallLabel(spaConfigurationData.mister), !spaConfigurationData.mister);
    html += "</tbody></table>";
    html += "<details><summary><b>Configuration metadata &amp; raw frame</b></summary>";
    html += "<dl class=\"config-kv\">";
    html += "<div class=\"kv-row\"><dt>magicNumber</dt><dd>" + String(spaConfigurationData.magicNumber) + "</dd></div>";
    html += "<div class=\"kv-row\"><dt>configuration CRC</dt><dd>" + String(spaConfigurationData.crc) + "</dd></div>";
    html += "</dl>";
    html += "<pre class=\"config-hex\">" +
            spaHexWordsUpper(spaConfigurationData.rawData, spaConfigurationData.rawDataLength, 48) + "</pre></details>";
  }
  html += "</section>";

  html += "<section class='panel' id='cfg-identity'><h1>Controller identity</h1>";
  html += "<p class=\"chart-caption\" style=\"margin:0 0 10px 0\">Decoded from the spa <strong>Information</strong> response "
          "(Balboa message type <code>0x24</code>). Field meanings match "
          "<a href=\"https://github.com/ccutrer/balboa_worldwide_app/blob/main/doc/protocol.md\" target=\"_blank\" rel=\"noopener\">protocol.md — Information Response</a>.</p>";
  if (spaInformationData.lastUpdate == 0)
  {
    html += "<p style=\"margin:0\"><em>Not received yet.</em> The gateway requests this after startup; wait on RS485 or check "
            "<a href='/state'>ESP State</a> → Spa Data Freshness → Information.</p>";
    html += "</section>";
  }
  else
  {
    html += "<dl class=\"config-kv\">";
    html += "<div class=\"kv-row\"><dt>lastUpdate</dt><dd>" + statusLastUpdateDisplayHtml(spaInformationData.lastUpdate) + "</dd></div>";
    html += "<div class=\"kv-row\"><dt>Software ID</dt><dd>" + spaConfigTrimCopy(spaInformationData.softwareID) + "</dd></div>";
    html += "<div class=\"kv-row\"><dt>System model</dt><dd>" + spaConfigTrimCopy(spaInformationData.model) + "</dd></div>";
    html += "<div class=\"kv-row\"><dt>Setup number</dt><dd>" + String(spaInformationData.setupNumber) + "</dd></div>";
    html += "<div class=\"kv-row\"><dt>Configuration signature</dt><dd>" + spaInformationConfigSignatureHex(spaInformationData) + "</dd></div>";
    html += "<div class=\"kv-row\"><dt>Heater voltage</dt><dd>" + spaInformationHeaterVoltageLabel(spaInformationData.voltage) + "</dd></div>";
    html += "<div class=\"kv-row\"><dt>Heater type</dt><dd>" + spaInformationHeaterTypeLabel(spaInformationData.heaterType) + "</dd></div>";
    html += "<div class=\"kv-row\"><dt>DIP switches</dt><dd>" + String(spaInformationData.dipSwitch) + "</dd></div>";
    html += "<div class=\"kv-row\"><dt>Frame CRC byte</dt><dd>" + String(spaInformationData.crc) + "</dd></div>";
    html += "</dl>";
    html += "<details><summary><b>Raw information frame (hex)</b></summary><pre class=\"config-hex\">" +
            spaHexWordsUpper(spaInformationData.rawData, spaInformationData.rawDataLength, 48) + "</pre></details>";
    html += "</section>";
  }

  html += "<section class='panel' id='cfg-filter'><h1>Filter configuration</h1>";
  {
    html += "<p class=\"chart-caption\" style=\"margin:0 0 10px 0\">Times use the spa panel clock time and format "
            "(<a href='/status'>configure on Spa Status</a>).</p>";
  }
  if (spaFilterSettingsData.lastUpdate == 0)
  {
    html += "<p style=\"margin:0\"><em>Filter settings not received yet — editing disabled until RS485 data is available.</em></p>";
  }
  else
  {
    const String f1Start = configTimeInputValue(spaFilterSettingsData.filt1Hour, spaFilterSettingsData.filt1Minute);
    const String f2Start = configTimeInputValue(spaFilterSettingsData.filt2Hour, spaFilterSettingsData.filt2Minute);
    const String filterTimeLang = configFilterTimeInputLangAttr(spaPanelClockFormatIs24Hour());
    html += "<div class=\"config-filter-strip\">";
    html += "<div class=\"config-filter-card\"><h2>Filter 1</h2>"
            "<div class=\"config-filter-fields\">"
            "<label for='cfgF1Start'>Start time</label>"
            "<input id='cfgF1Start' type='time' step='900'" + filterTimeLang + " value='" + f1Start + "' />"
            "<label>Duration</label>"
            + configFilterDurationFieldsHtml("cfgF1DurH", "cfgF1DurM", spaFilterSettingsData.filt1DurationHour,
                                             spaFilterSettingsData.filt1DurationMinute) +
            "</div></div>";
    html += "<div class=\"config-filter-card\"><h2>Filter 2</h2>"
            "<label class='config-filter-enable'><input type='checkbox' id='cfgF2Enable'" +
            String(spaFilterSettingsData.filt2Enable ? " checked" : "") + " /> Enable filter 2</label>"
            "<div class=\"config-filter-fields\">"
            "<label for='cfgF2Start'>Start time</label>"
            "<input id='cfgF2Start' type='time' step='900'" + filterTimeLang + " value='" + f2Start + "' />"
            "<label>Duration</label>"
            + configFilterDurationFieldsHtml("cfgF2DurH", "cfgF2DurM", spaFilterSettingsData.filt2DurationHour,
                                             spaFilterSettingsData.filt2DurationMinute) +
            "</div></div>";
    html += "</div>";
    html += "<button class='equip-btn' type='button' id='cfgFilterSaveBtn'>Save filter schedule</button>";
    html += "<p id='cfgFilterStatus' style='margin:8px 0 0 0;font-size:14px;color:var(--muted)'></p>";
  }
  html += "<dl class=\"config-kv\">";
  html += "<div class=\"kv-row\"><dt>lastUpdate</dt><dd id='cfgFilterLastUpdate'>" + statusLastUpdateDisplayHtml(spaFilterSettingsData.lastUpdate) + "</dd></div>";
  if (spaFilterSettingsData.lastUpdate != 0)
  {
    html += "<div class=\"kv-row\"><dt>Filter 2 enabled</dt><dd id='cfgFilter2Enabled'>" + String(spaFilterSettingsData.filt2Enable ? "yes" : "no") + "</dd></div>";
  }
  html += "</dl>";
  if (spaFilterSettingsData.lastUpdate != 0)
  {
    html += "<details><summary><b>Raw filter-settings frame (hex)</b></summary><pre class=\"config-hex\" id='cfgFilterRawHex'>" +
            spaHexWordsUpper(spaFilterSettingsData.rawData, spaFilterSettingsData.rawDataLength, 48) + "</pre></details>";
  }
  html += "</section>";

  html += "<section class='panel' id='cfg-preferences'><h1>Panel preferences</h1>";
  html += "<p class=\"chart-caption\" style=\"margin:0 0 10px 0\">From the spa <strong>Preferences</strong> response "
          "(Balboa settings request <code>0x22</code> / <code>0x08</code>). MQTT: "
          "<code>Spa/&lt;gateway&gt;/preferences/</code>.</p>";
  html += "<div class=\"config-filter-card\" style=\"margin:0 0 12px 0\"><h2>Maintenance reminders</h2>";
  html += "<p style=\"margin:0 0 8px 0;font-size:14px\">Controls whether the topside panel shows scheduled messages "
          "(Clean Filter, Check pH, Change Water, etc.). This is the same <strong>Reminders</strong> setting on the spa settings menu.</p>";
  html += "<dl class=\"config-kv\" style=\"margin:0 0 10px 0\"><div class=\"kv-row\"><dt>Current</dt><dd id=\"cfgPrefsRemindersVal\">";
  if (spaPreferencesData.lastUpdate == 0)
  {
    html += "<em>Not received yet</em>";
  }
  else
  {
    html += spaPreferencesRemindersText(spaPreferencesData.reminders);
  }
  html += "</dd></div></dl>";
  html += "<div class=\"config-backup-actions\" style=\"margin:0\">";
  html += "<button class=\"equip-btn\" type=\"button\" id=\"cfgPrefsRemindersOnBtn\"";
  if (spaPreferencesData.lastUpdate == 0 || spaPreferencesRemindersEnabled(spaPreferencesData.reminders))
  {
    html += " disabled";
  }
  html += ">Turn reminders on</button>";
  html += "<button class=\"equip-btn\" type=\"button\" id=\"cfgPrefsRemindersOffBtn\"";
  if (spaPreferencesData.lastUpdate == 0 || !spaPreferencesRemindersEnabled(spaPreferencesData.reminders))
  {
    html += " disabled";
  }
  html += ">Turn reminders off</button></div>";
  html += "<p id=\"cfgPrefsRemindersStatus\" class=\"chart-caption\" style=\"margin:8px 0 0 0\">";
  if (spaPreferencesData.lastUpdate == 0)
  {
    html += "Waiting for preferences from the spa controller (requested automatically after connect).";
  }
  html += "</p></div>";
  if (spaPreferencesData.lastUpdate == 0)
  {
    html += "</section>";
  }
  else
  {
    html += "<dl class=\"config-kv\">";
    html += "<div class=\"kv-row\"><dt>lastUpdate</dt><dd id=\"cfgPrefsLastUpdate\">" +
            statusLastUpdateDisplayHtml(spaPreferencesData.lastUpdate) + "</dd></div>";
    html += "<div class=\"kv-row\"><dt>remindersRaw</dt><dd>0x";
    if (spaPreferencesData.reminders < 16)
    {
      html += "0";
    }
    html += String(spaPreferencesData.reminders, HEX);
    html += " (" + String(spaPreferencesData.reminders) + ")</dd></div>";
    html += "<div class=\"kv-row\"><dt>tempScale</dt><dd>" + String(spaPreferencesData.tempScale) + "</dd></div>";
    html += "<div class=\"kv-row\"><dt>clockMode</dt><dd>" + String(spaPreferencesData.clockMode) + "</dd></div>";
    html += "<div class=\"kv-row\"><dt>cleanupCycle</dt><dd>" + String(spaPreferencesData.cleanupCycle) + "</dd></div>";
    html += "<div class=\"kv-row\"><dt>dolphinAddress</dt><dd>" + String(spaPreferencesData.dolphinAddress) + "</dd></div>";
    html += "<div class=\"kv-row\"><dt>m8AI</dt><dd>" + String(spaPreferencesData.m8AI) + "</dd></div>";
    html += "</dl>";
    html += "<details><summary><b>Raw preferences frame (hex)</b></summary><pre class=\"config-hex\">" +
            spaHexWordsUpper(spaPreferencesData.rawData, spaPreferencesData.rawDataLength, 48) + "</pre></details>";
    html += "</section>";
  }

  html += "<section class='panel config-span-full' id='cfg-history'><h1>Spa controller history</h1>";
  html += "<p class=\"chart-caption\" style=\"margin:0 0 12px 0\">Historical events stored on the spa pack (Balboa fault log). "
          "This is not live equipment state on <a href='/status'>Spa Status</a>, and not the ESP gateway diagnostic ring on "
          "<a href='/state'>ESP State</a> / <code>GET /api/diagnostics</code> &rarr; <code>faultLog</code>.</p>";
  if (spaFaultLogData.lastUpdate == 0)
  {
    html += "<p style=\"margin:0\"><em>Latest event not received yet.</em> The gateway requests this after startup.</p>";
  }
  else
  {
    const String eventText = spaFaultMessageForCode(spaFaultLogData.faultCode, spaFaultLogData.totEntry);
    html += "<div class=\"config-fault-latest\" id=\"cfgFaultLatestCard\">";
    html += "<h2>Latest event</h2>";
    html += "<p class=\"config-fault-headline\"><span id=\"cfgFaultLatestEventText\">";
    html += eventText;
    html += "</span>";
    appendConfigFaultSeverityBadge(html, spaFaultLogData.faultCode);
    html += "</p>";
    html += "<p class=\"config-fault-meta\">Logged on spa panel clock: <span id=\"cfgFaultLatestOccurred\">";
    html += spaFormatFaultLogTime(spaFaultLogData);
    html += "</span></p>";
    html += "<p class=\"config-fault-meta\">Entry <span id=\"cfgFaultLatestEntry\">";
    html += String(spaFaultLogData.currEntry);
    html += "</span> of <span id=\"cfgFaultLatestTotEntry\">";
    html += String(spaFaultLogData.totEntry);
    html += "</span> log slots &middot; Code <span id=\"cfgFaultLatestCode\">";
    html += String(spaFaultLogData.faultCode);
    html += "</span></p>";
    html += "<p class=\"config-fault-meta\">Last fetched by gateway: <span id=\"cfgFaultLatestFetched\">";
    html += statusLastUpdateDisplayHtml(spaFaultLogData.lastUpdate);
    html += "</span></p>";
    html += "<details><summary><b>Technical details</b></summary>";
    html += "<dl class=\"config-kv\">";
    html += "<div class=\"kv-row\"><dt>CRC byte</dt><dd>" + String(spaFaultLogData.crc) + "</dd></div>";
    html += "</dl>";
    html += "<pre class=\"config-hex\">" +
            spaHexWordsUpper(spaFaultLogData.rawData, spaFaultLogData.rawDataLength, 48) + "</pre></details>";
    html += "</div>";
    html += "<details id=\"cfgFaultHistoryDetails\"><summary><b>View full event history</b></summary>";
    html += "<p class=\"chart-caption\" style=\"margin:8px 0\">Loads every stored log entry from the spa controller over RS485 "
            "(may take up to a minute).</p>";
    html += "<button class=\"equip-btn\" type=\"button\" id=\"cfgFaultHistoryLoadBtn\">Load history from spa</button>";
    html += "<p id=\"cfgFaultHistoryStatus\" class=\"chart-caption\" style=\"margin:8px 0 0 0\"></p>";
    html += "<table class=\"config-fault-history\" id=\"cfgFaultHistoryTable\" style=\"display:none\">";
    html += "<thead><tr><th>When (panel clock)</th><th>Event</th><th>Code</th><th>Severity</th></tr></thead>";
    html += "<tbody id=\"cfgFaultHistoryBody\"></tbody></table></details>";
  }
  html += "<details style=\"margin-top:12px\"><summary><b>Developer: undecoded settings (0x04)</b></summary>";
  html += "<p class=\"chart-caption\" style=\"margin:8px 0\">Raw Balboa settings sub-block; not decoded for display yet.</p>";
  if (spaSettings0x04Data.lastUpdate == 0)
  {
    html += "<p style=\"margin:0\"><em>Not received yet.</em></p>";
  }
  else
  {
    html += "<p style=\"margin:0 0 6px 0\"><span style=\"color:var(--muted)\">lastUpdate:</span> " +
            statusLastUpdateDisplayHtml(spaSettings0x04Data.lastUpdate) +
            ", <span style=\"color:var(--muted)\">CRC byte:</span> " + String(spaSettings0x04Data.crc) + "</p>";
    html += "<pre class=\"config-hex\">" +
            spaHexWordsUpper(spaSettings0x04Data.rawData, spaSettings0x04Data.rawDataLength, 48) + "</pre>";
  }
  html += "</details></section>";

  html += "<section class='panel config-span-full' id='cfg-littlefs'><h1>LittleFS configuration</h1>";
  html += "<p class='chart-caption'>Load on demand to avoid large payloads on weak links.</p>";
  html += "<button class='equip-btn' type='button' id='cfgLoadLittleFsBtn'>Load LittleFS file list</button>";
  html += "<ul id='cfgLittleFsContainer'></ul>";
  html += "</section></div>";

  html += "<script>(function(){var btn=document.getElementById('cfgLoadLittleFsBtn');var box=document.getElementById('cfgLittleFsContainer');"
          "if(btn&&box){btn.addEventListener('click',function(){if(btn.disabled)return;btn.disabled=true;btn.textContent='Loading...';"
          "fetch('/api/state/littlefs',{cache:'no-store'}).then(function(r){if(!r.ok)throw new Error('http');return r.json();}).then(function(j){"
          "box.innerHTML='<li>'+(j&&j.html?j.html:'(empty)')+'</li>';btn.textContent='LittleFS loaded';}).catch(function(){btn.disabled=false;btn.textContent='Retry LittleFS load';});});}"
          "function cfgParseHm(v){if(!v||v.indexOf(':')<0)return{h:0,m:0};var p=v.split(':');return{h:parseInt(p[0],10)||0,m:parseInt(p[1],10)||0};}"
          "function cfgParseDur(hId,mId){var hEl=document.getElementById(hId);var mEl=document.getElementById(mId);"
          "var h=hEl?parseInt(hEl.value,10):0;var m=mEl?parseInt(mEl.value,10):0;return{h:isNaN(h)?0:h,m:isNaN(m)?0:m};}"
          "function cfgFilterPayload(){var s1=cfgParseHm(document.getElementById('cfgF1Start')&&document.getElementById('cfgF1Start').value);"
          "var d1=cfgParseDur('cfgF1DurH','cfgF1DurM');"
          "var s2=cfgParseHm(document.getElementById('cfgF2Start')&&document.getElementById('cfgF2Start').value);"
          "var d2=cfgParseDur('cfgF2DurH','cfgF2DurM');"
          "var en=document.getElementById('cfgF2Enable')&&document.getElementById('cfgF2Enable').checked;"
          "var f2s={startHour:s2.h,startMinute:s2.m,durationHour:en?d2.h:0,durationMinute:en?d2.m:0};"
          "return{filter1:{startHour:s1.h,startMinute:s1.m,durationHour:d1.h,durationMinute:d1.m},"
          "filter2:{enabled:!!en,startHour:en?s2.h:0,startMinute:en?s2.m:0,durationHour:f2s.durationHour,durationMinute:f2s.durationMinute}};}"
          "function cfgFilter2Matches(reqF2,gotF2){if(!reqF2||!gotF2)return false;"
          "if(!!reqF2.enabled!==!!gotF2.enabled)return false;if(!gotF2.enabled)return true;"
          "return reqF2.startHour===gotF2.startHour&&reqF2.startMinute===gotF2.startMinute&&"
          "reqF2.durationHour===gotF2.durationHour&&reqF2.durationMinute===gotF2.durationMinute;}"
          "function cfgFilterMatches(req,got){if(!req||!got||!req.filter1||!got.filter1||!req.filter2||!got.filter2)return false;"
          "return req.filter1.startHour===got.filter1.startHour&&req.filter1.startMinute===got.filter1.startMinute&&"
          "req.filter1.durationHour===got.filter1.durationHour&&req.filter1.durationMinute===got.filter1.durationMinute&&"
          "cfgFilter2Matches(req.filter2,got.filter2);}"
          "function cfgFilterSyncMeta(j){var f2=document.getElementById('cfgFilter2Enabled');"
          "if(f2&&j&&j.filter2)f2.textContent=j.filter2.enabled?'yes':'no';}"
          "var saveBtn=document.getElementById('cfgFilterSaveBtn');if(saveBtn){saveBtn.addEventListener('click',function(){"
          "var st=document.getElementById('cfgFilterStatus');var req=cfgFilterPayload();var baseLast=0;"
          "fetch('/api/config/filter',{cache:'no-store'}).then(function(r){return r.json();}).then(function(j){baseLast=j.lastUpdate||0;"
          "return fetch('/api/config/filter',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(req)});"
          "}).then(function(r){return r.json();}).then(function(j){if(!j.accepted){if(st)st.textContent='Save rejected: '+(j.reason||'unknown');return;}"
          "if(st)st.textContent='Queued — verifying readback…';var tries=0;var timer=setInterval(function(){tries++;"
          "fetch('/api/config/filter',{cache:'no-store'}).then(function(r){return r.json();}).then(function(j){"
          "var got={filter1:j.filter1,filter2:j.filter2};if(cfgFilterMatches(req,got)||(j.lastUpdate&&j.lastUpdate>baseLast)){"
          "clearInterval(timer);cfgFilterSyncMeta(j);if(st)st.textContent='Saved — verified on controller.';}else if(tries>=12){clearInterval(timer);"
          "if(st)st.textContent='Queued — readback mismatch (controller may have ignored write).';}}).catch(function(){});},2000);"
          "}).catch(function(e){if(st)st.textContent='Save failed.';});});}"
          "var exBtn=document.getElementById('cfgExportBtn');if(exBtn){exBtn.addEventListener('click',function(){"
          "fetch('/api/config/export',{cache:'no-store'}).then(function(r){if(!r.ok)throw new Error('http');return r.blob();}).then(function(b){"
          "var a=document.createElement('a');a.href=URL.createObjectURL(b);a.download='spa-config.json';a.click();URL.revokeObjectURL(a.href);"
          "}).catch(function(){});});}"
          "function cfgImportRun(dry){var f=document.getElementById('cfgImportFile');var prev=document.getElementById('cfgImportPreview');"
          "var force=document.getElementById('cfgImportForce')&&document.getElementById('cfgImportForce').checked;if(!f||!f.files||!f.files[0]){"
          "if(prev){prev.style.display='block';prev.textContent='Choose a JSON file first.';}return;}"
          "var reader=new FileReader();reader.onload=function(){try{var data=JSON.parse(reader.result);data.dryRun=!!dry;data.force=!!force;"
          "fetch('/api/config/import',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)})"
          ".then(function(r){return r.json();}).then(function(j){if(prev){prev.style.display='block';prev.textContent=JSON.stringify(j,null,2);}"
          "if(!dry&&j.accepted&&!j.blocked){setTimeout(function(){location.reload();},1500);}}).catch(function(){"
          "if(prev){prev.style.display='block';prev.textContent='Import request failed.';}});}catch(e){if(prev){prev.style.display='block';"
          "prev.textContent='Invalid JSON file.';}}};reader.readAsText(f.files[0]);}"
          "var pv=document.getElementById('cfgImportPreviewBtn');if(pv){pv.addEventListener('click',function(){cfgImportRun(true);});}"
          "var ap=document.getElementById('cfgImportApplyBtn');if(ap){ap.addEventListener('click',function(){cfgImportRun(false);});}"
          "function cfgSyncPrefsUi(j){if(!j)return;var val=document.getElementById('cfgPrefsRemindersVal');"
          "var onBtn=document.getElementById('cfgPrefsRemindersOnBtn');var offBtn=document.getElementById('cfgPrefsRemindersOffBtn');"
          "var st=document.getElementById('cfgPrefsRemindersStatus');"
          "function cfgRemindersOn(j){if(!j||!j.ready)return false;return typeof j.remindersEnabled==='boolean'?j.remindersEnabled:((Number(j.reminders||0)&1)!==0);}"
          "if(val){if(j.ready){val.textContent=j.remindersText||'';}else{val.innerHTML='<em>Not received yet</em>';}}"
          "if(onBtn)onBtn.disabled=!j.ready||cfgRemindersOn(j);if(offBtn)offBtn.disabled=!j.ready||!cfgRemindersOn(j);"
          "if(st&&!st.dataset.busy){if(!j.ready)st.textContent='Waiting for preferences from the spa controller (requested automatically after connect).';"
          "else if(!st.textContent)st.textContent='';}}"
          "function cfgSetReminders(enabled){var st=document.getElementById('cfgPrefsRemindersStatus');"
          "var label=enabled?'ON':'OFF';"
          "if(!confirm('Turn panel maintenance reminders '+label+'?\\n\\nThis changes the spa topside Reminders setting (Clean Filter, Check pH, Change Water, etc.).')){"
          "if(st)st.textContent='Reminders change canceled.';return;}"
          "if(st){st.dataset.busy='1';st.textContent='Sending to spa…';}"
          "var baseLast=0;fetch('/api/config/preferences',{cache:'no-store'}).then(function(r){return r.json();}).then(function(j){"
          "baseLast=j.lastUpdate||0;return fetch('/api/config/preferences',{method:'POST',headers:{'Content-Type':'application/json'},"
          "body:JSON.stringify({reminders:enabled?1:0})});}).then(function(r){return r.json();}).then(function(j){"
          "if(!j.accepted){if(st){st.dataset.busy='';st.textContent='Save rejected: '+(j.reason||'unknown');}return;}"
          "if(st)st.textContent='Queued — verifying readback…';var tries=0;var timer=setInterval(function(){tries++;"
          "fetch('/api/config/preferences',{cache:'no-store'}).then(function(r){return r.json();}).then(function(j){"
          "if(j.ready&&(cfgRemindersOn(j)===enabled||(j.lastUpdate&&j.lastUpdate>baseLast))){"
          "clearInterval(timer);cfgSyncPrefsUi(j);if(st){st.dataset.busy='';st.textContent='Saved — verified on controller.';}}"
          "else if(tries>=12){clearInterval(timer);if(st){st.dataset.busy='';st.textContent='Queued — readback not confirmed yet (reload page in a moment).';}}"
          "}).catch(function(){});},2000);}).catch(function(){if(st){st.dataset.busy='';st.textContent='Save failed.';}});}"
          "var pOn=document.getElementById('cfgPrefsRemindersOnBtn');if(pOn){pOn.addEventListener('click',function(){cfgSetReminders(true);});}"
          "var pOff=document.getElementById('cfgPrefsRemindersOffBtn');if(pOff){pOff.addEventListener('click',function(){cfgSetReminders(false);});}"
          "fetch('/api/config/preferences',{cache:'no-store'}).then(function(r){return r.json();}).then(cfgSyncPrefsUi).catch(function(){});"
          "setInterval(function(){fetch('/api/config/preferences',{cache:'no-store'}).then(function(r){return r.json();}).then(function(j){"
          "if(j&&j.ready)cfgSyncPrefsUi(j);}).catch(function(){});},5000);"
          "function cfgFaultSevClass(sev){if(sev==='alert')return 'config-fault-sev config-fault-sev--alert';"
          "if(sev==='warning')return 'config-fault-sev config-fault-sev--warning';return 'config-fault-sev config-fault-sev--info';}"
          "function cfgRenderFaultHistory(j){var tbody=document.getElementById('cfgFaultHistoryBody');"
          "var table=document.getElementById('cfgFaultHistoryTable');if(!tbody||!table||!j||!j.entries)return;"
          "tbody.innerHTML='';j.entries.forEach(function(row){var tr=document.createElement('tr');"
          "tr.innerHTML='<td>'+(row.occurredText||'')+'</td><td>'+(row.eventText||'')+'</td><td>'+(row.faultCode||'')+'</td>'"
          "+'<td><span class=\"'+cfgFaultSevClass(row.severity)+'\">'+(row.severity||'')+'</span></td>';tbody.appendChild(tr);});"
          "table.style.display=j.entries.length?'table':'none';}"
          "function cfgPollFaultHistory(){fetch('/api/config/fault-log/history',{cache:'no-store'}).then(function(r){return r.json();}).then(function(j){"
          "var st=document.getElementById('cfgFaultHistoryStatus');var btn=document.getElementById('cfgFaultHistoryLoadBtn');"
          "if(typeof j.loading!=='boolean'){if(st)st.textContent='Unexpected history API response — reload the page and try again.';"
          "if(btn)btn.disabled=false;return;}"
          "if(st){if(j.loading){var total=j.total||'?';var n=(typeof j.pendingEntry==='number'?j.pendingEntry:j.progress||0)+1;"
          "if(j.total&&n>j.total)n=j.total;st.textContent='Loading entry '+n+' of '+total+'…';"
          "if(j.entries&&j.entries.length)cfgRenderFaultHistory(j);}"
          "else if(j.error){if(j.entries&&j.entries.length){cfgRenderFaultHistory(j);"
          "st.textContent='History load incomplete — showing '+j.entries.length+' events collected so far.';}"
          "else{st.textContent='History load failed or timed out — try again.';}"
          "if(btn)btn.disabled=false;}"
          "else if(j.complete){st.textContent='Loaded '+((j.entries&&j.entries.length)||0)+' events from the spa controller.';"
          "cfgRenderFaultHistory(j);if(btn){btn.disabled=false;btn.textContent='Reload history from spa';}}}"
          "if(j.loading){setTimeout(cfgPollFaultHistory,2000);}}).catch(function(){"
          "var st=document.getElementById('cfgFaultHistoryStatus');if(st)st.textContent='History request failed.';"
          "var btn=document.getElementById('cfgFaultHistoryLoadBtn');if(btn)btn.disabled=false;});}"
          "function cfgLoadFaultHistory(){var btn=document.getElementById('cfgFaultHistoryLoadBtn');"
          "var st=document.getElementById('cfgFaultHistoryStatus');var tbody=document.getElementById('cfgFaultHistoryBody');"
          "var table=document.getElementById('cfgFaultHistoryTable');if(tbody)tbody.innerHTML='';if(table)table.style.display='none';"
          "if(btn)btn.disabled=true;"
          "if(st)st.textContent='Starting history load…';"
          "fetch('/api/config/fault-log/history',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action:'start'})})"
          ".then(function(r){return r.json();}).then(function(j){if(!j.accepted){if(st)st.textContent='Could not start: '+(j.reason||'unknown');"
          "if(btn)btn.disabled=false;return;}cfgPollFaultHistory();}).catch(function(){if(st)st.textContent='Failed to start history load.';"
          "if(btn)btn.disabled=false;});}"
          "var fBtn=document.getElementById('cfgFaultHistoryLoadBtn');if(fBtn){fBtn.addEventListener('click',cfgLoadFaultHistory);}"
          "})();</script>";

  html += "</main></div></body></html>";
  String etag = String("W/\"cfg-") + String(VERSION) + "-" + String(BUILD) + "-" + String(spaConfigurationData.lastUpdate) + "-" +
              String(spaFilterSettingsData.lastUpdate) + "-" + String(spaFilterSettingsData.filt1Hour) + "-" +
              String(spaFilterSettingsData.filt1Minute) + "-" + String(spaFilterSettingsData.filt2Enable) + "-" +
              String(spaInformationData.lastUpdate) + "-" +
              String(spaPreferencesData.lastUpdate) + "-" + String(spaSettings0x04Data.lastUpdate) + "-" +
              String(spaFaultLogData.lastUpdate) + "\"";
  logPortalHtmlMissingGlobalCss(sawPortalCss, html.length(), "/config");
  Log.verbose(F("[Web]: /config assembled len=%u slabs=%u freeHeap=%u maxAlloc=%u healthy=%u" CR),
              static_cast<unsigned>(html.length()), static_cast<unsigned>(html.slabCount()),
              static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()),
              html.healthy() ? 1u : 0u);
  sendHtmlChunksWithEtag(request, html, etag);
  Log.verbose("[Web]: handleConfig %p %s %s" CR, request->client()->remoteIP(), request->methodToString(), request->url().c_str());
}

time_t testLastCheckedTime = getTime();

void handleState(AsyncWebServerRequest *request)
{
  if (!portalLargePageTryBegin(request, "/state"))
  {
    return;
  }
  // Log.verbose(F("[Web]: handleStatus()" CR));
  // Keep CSS as a flash/rodata literal and append through PortalHtmlChunks (do not heap-allocate
  // a large Arduino String first — that can fail under fragmentation while slabs still succeed).
  const char *stateEnhancements = "<style>.state-grid{display:grid;grid-template-columns:1fr;gap:14px;}@media (min-width:980px){.state-grid{grid-template-columns:1fr 1fr;}.state-grid .panel{margin-bottom:0;}}.diag-badge{display:inline-block;padding:2px 8px;border-radius:999px;font-size:.88rem;}.state-toolbar{display:flex;justify-content:space-between;align-items:center;gap:12px;flex-wrap:wrap;margin:0 0 10px 0;}.state-freshness{width:100%;border-collapse:collapse;margin-top:8px;}.state-freshness th,.state-freshness td{padding:8px;border-bottom:1px solid var(--border);text-align:left;vertical-align:top;}.state-freshness th{font-size:13px;color:var(--muted);}body .advanced-panel{display:none;}body.show-advanced .advanced-panel{display:block;}body .advanced-only{display:none;}body.show-advanced li.advanced-only{display:list-item;}body.show-advanced .sys-advanced-block{display:grid;}button.fw-check-btn{background:var(--panel)!important;color:var(--text)!important;border:1px solid var(--border)!important;flex:0 0 auto!important;width:auto!important;min-width:auto!important;padding:8px 14px!important;font-size:14px!important;font-weight:600!important;}button.fw-danger-btn{color:#991b1b!important;border-color:#fca5a5!important;background:#fef2f2!important;}#fwUpdateResult.fw-update-msg{display:block;width:100%;max-width:100%;margin:0;font-size:14px;font-weight:600;line-height:1.35;color:var(--muted);}.fw-compare{display:flex;flex-direction:column;gap:12px;margin:0;}.fw-compare-cols{display:grid;grid-template-columns:1fr 1fr 1fr;gap:12px;align-items:start;}@media (max-width:560px){.fw-compare-cols{grid-template-columns:1fr;}}.fw-compare-item{display:flex;flex-direction:column;gap:6px;min-width:0;}.fw-compare-label{font-size:12px;font-weight:600;color:var(--muted);letter-spacing:.02em;}.fw-actions{display:flex;flex-wrap:nowrap;align-items:center;gap:10px;width:100%;box-sizing:border-box;overflow-x:auto;-webkit-overflow-scrolling:touch;}.fw-actions .fw-check-btn{flex:0 0 auto;}.fw-actions .gh-sponsor-embed{flex:0 0 auto;flex-shrink:0;line-height:0;align-self:center;}.fw-pill{display:inline-block;border-radius:999px;padding:3px 10px;font-size:12px;font-weight:700;line-height:1.2;border:1px solid var(--border);background:var(--surface-2);color:var(--text);}.fw-pill-current{background:#edf7ff;color:var(--heading);border-color:#b7d6f2;}.fw-pill-latest{background:var(--surface-2);color:var(--muted);}.fw-pill-branch{background:#fffbeb;color:#92400e;border-style:dashed;border-color:#fcd34d;}.sub-card{border:1px solid var(--border);background:var(--surface-2);border-radius:10px;padding:10px 12px;margin:8px 0;}.sub-card-title{font-size:13px;font-weight:700;letter-spacing:.01em;color:var(--muted);text-transform:uppercase;margin:0 0 8px 0;}.sub-card-row{display:flex;flex-wrap:wrap;align-items:center;gap:10px;}.fw-repo-links{display:flex;flex-wrap:nowrap;align-items:center;gap:0 6px;margin-top:8px;font-size:13px;overflow-x:auto;-webkit-overflow-scrolling:touch;white-space:nowrap;}.fw-repo-links a{flex:0 0 auto;}.fw-repo-sep{color:var(--muted);opacity:.7;flex:0 0 auto;user-select:none;}.gh-sponsor-embed iframe{display:block;border:0;border-radius:6px;vertical-align:middle;}.wifi-hero{display:flex;flex-wrap:wrap;align-items:center;gap:10px 14px;padding:10px 12px;border:1px solid var(--border);border-radius:10px;background:var(--surface-2);margin-bottom:8px;}.wifi-hero__status{flex:0 0 auto;}.wifi-hero__net{flex:1 1 120px;min-width:0;}.wifi-hero__ssid{font-weight:700;font-size:1rem;line-height:1.25;overflow-wrap:anywhere;}.wifi-hero__host{font-size:13px;color:var(--muted);overflow-wrap:anywhere;margin-top:2px;}.wifi-hero__signal{flex:0 0 auto;text-align:right;min-width:72px;}.wifi-hero__rssi{font-size:1.15rem;font-weight:700;line-height:1.2;}#wf-rssi,#wf-quality{font-weight:700;}.wifi-meta{font-size:12px;color:var(--muted);margin:0 0 10px 0;overflow-wrap:anywhere;line-height:1.45;}.wifi-meta__sep{opacity:.55;padding:0 5px;}.wifi-body{display:grid;grid-template-columns:1fr 1fr;gap:12px;align-items:start;}@media (max-width:520px){.wifi-body{grid-template-columns:1fr;}}.wifi-block-title{font-size:13px;font-weight:700;color:var(--muted);text-transform:uppercase;letter-spacing:.01em;margin:0 0 8px 0;}.wifi-kv{display:grid;grid-template-columns:auto 1fr;gap:4px 12px;margin:0;font-size:14px;align-items:baseline;}.wifi-kv dt{color:var(--muted);font-weight:600;margin:0;}.wifi-kv dd{margin:0;overflow-wrap:anywhere;font-family:ui-monospace,Menlo,Consolas,monospace;font-size:13px;}.wifi-signal-card{border:1px solid var(--border);background:var(--surface-2);border-radius:10px;padding:10px 12px;}.wifi-signal-row{display:flex;justify-content:space-between;align-items:baseline;gap:8px;margin-bottom:6px;font-size:14px;}.wifi-signal-row__label{color:var(--muted);font-weight:600;flex:0 0 auto;}.wifi-signal-row__value{font-weight:600;text-align:right;overflow-wrap:anywhere;}.wifi-signal-caption{font-size:12px;color:var(--muted);margin:8px 0 4px 0;}.sys-hero{display:flex;flex-wrap:wrap;align-items:center;gap:10px 14px;padding:10px 12px;border:1px solid var(--border);border-radius:10px;background:var(--surface-2);margin-bottom:8px;}.sys-hero__uptime,.sys-hero__time,.sys-hero__rs485{flex:1 1 100px;min-width:0;}.sys-hero__uptime-val{font-size:1.15rem;font-weight:700;line-height:1.2;}.sys-hero__time-val{font-size:14px;font-weight:600;overflow-wrap:anywhere;}.sys-hero__rs485{text-align:right;}.sys-meta{font-size:13px;color:var(--muted);margin:0 0 10px 0;overflow-wrap:anywhere;line-height:1.45;}.sys-meta__label{display:block;font-size:12px;font-weight:600;color:var(--muted);margin-bottom:2px;}.sys-advanced-block{display:none;grid-template-columns:1fr 1fr;gap:12px;margin-top:8px;align-items:start;}@media (max-width:520px){.sys-advanced-block{grid-template-columns:1fr;}}.sys-stat-tiles{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;}@media (max-width:420px){.sys-stat-tiles{grid-template-columns:1fr;}}.sys-stat-tile{display:flex;flex-direction:column;gap:4px;min-width:0;}.sys-stat-val{font-family:ui-monospace,Menlo,Consolas,monospace;font-size:13px;font-weight:600;overflow-wrap:anywhere;}.sys-build-def{font-family:ui-monospace,Menlo,Consolas,monospace;font-size:12px;overflow-wrap:anywhere;margin:6px 0 0 0;line-height:1.4;}.rs485-hint{font-size:13px;color:var(--muted);margin:0 0 6px 0;line-height:1.4;}.rs485-deep-meta{font-size:12px;color:var(--muted);margin:0 0 10px 0;overflow-wrap:anywhere;line-height:1.45;}</style>";
  PortalHtmlChunks html;
  html = F("<html>");
  const bool sawPortalCss = appendPortalHead(html, "ESP State");
  html += stateEnhancements;
  html += F("<body><a class='skip-link' href='#mainContent'>Skip to main content</a><div class='page'>");
  html += webMenuState;
  html += F("<main id='mainContent'>");
  html += ePaper;
  html += "<section class='panel'><div class='state-toolbar'><h1 style='margin:0'>ESP State</h1><label style='font-size:14px'><input id='toggleAdvanced' type='checkbox'/> Show advanced diagnostics</label></div>";
  html += "<p style='margin:0 0 10px 0;font-size:14px;color:var(--muted)'>Signal-first layout keeps daily health visible. <b>Reboot gateway</b> and deeper diagnostics are under <b>Show advanced diagnostics</b>. API shortcuts are below for direct endpoint access.</p></section>";
  html += "<div class='state-grid'><section class='panel'><h1>System Health</h1>";
#ifdef LOCAL_CLIENT
  String rsHealth = String(rs485HealthCode());
  unsigned long rs485LastValidAgeMs = 0;
  if (rs485Stats.lastValidFrameMs > 0)
  {
    rs485LastValidAgeMs = (millis() >= rs485Stats.lastValidFrameMs) ? (millis() - rs485Stats.lastValidFrameMs) : 0;
  }
#endif
  html += "<div class='sys-hero'><div class='sys-hero__uptime'><span class='fw-compare-label'>Uptime</span>"
          "<div class='sys-hero__uptime-val'>" + formatNumberWithCommas(millis() / 1000) + " s</div></div>";
  html += "<div class='sys-hero__time'><span class='fw-compare-label'>Current time</span>"
          "<div class='sys-hero__time-val'>" + webWallClockDisplayHtml(getTime()) + "</div></div>";
#ifdef LOCAL_CLIENT
  html += "<div class='sys-hero__rs485'><span class='fw-compare-label'>RS485</span>"
          "<div style='margin-top:4px'><span class='diag-badge' style='font-weight:700;color:#fff;background:" + rs485HealthColor(rsHealth) + "'>" + rs485HealthLabel(rsHealth) + "</span></div></div>";
#endif
  html += "</div>";
#ifdef LOCAL_CLIENT
  if (rs485SafeModeActive())
  {
    html += "<p class='sys-meta' style='margin-top:10px;padding:10px;border-radius:8px;background:#fff3e0;border:1px solid #ef6c00'>"
            "<b>RS485 safe mode:</b> UART is skipped so Wi‑Fi/OTA stay reachable (" +
            String(rs485SafeModeReason()) + "). "
            "Fix pin config, then <code>POST /api/rs485/retry</code> or power-cycle.</p>";
  }
#endif
  {
    const String restartComposite = getLastRestartReason();
    const String restartIntent = getLastRestartIntent();
    html += "<p class='sys-meta'><span class='sys-meta__label'>Restart reason</span>" + restartComposite + "</p>";
    if (restartIntent.length() > 0 && restartComposite.indexOf(restartIntent) >= 0)
    {
      html += "<p class='sys-meta' style='margin-top:4px'><span class='sys-meta__label'>Restart intent</span>" + restartIntent + "</p>";
    }
  }
  {
    String fwPillDisplay = String(VERSION);
    if (fwPillDisplay.length() > 0 && fwPillDisplay.charAt(0) != 'v' && fwPillDisplay.charAt(0) != 'V')
    {
      fwPillDisplay = String("v") + fwPillDisplay;
    }
    html += "<div class='sub-card'><p class='sub-card-title'>Firmware Update</p>"
            "<div class='fw-compare'>"
            "<div class='fw-compare-cols'>"
            "<div class='fw-compare-item'><span class='fw-compare-label'>This gateway</span>"
            "<span id=\"fwCurrentBadge\" class=\"fw-pill fw-pill-current\">" + fwPillDisplay + "</span></div>"
            "<div class='fw-compare-item'><span class='fw-compare-label'>Latest release</span>"
            "<span id=\"fwLatestReleaseBadge\" class=\"fw-pill fw-pill-latest\">&#8212;</span></div>"
            "<div class='fw-compare-item'><span class='fw-compare-label'>" + String(FIRMWARE_REPO_DEFAULT_BRANCH) + " branch</span>"
            "<span id=\"fwLatestBranchBadge\" class=\"fw-pill fw-pill-branch\">&#8212;</span></div></div>"
            "<div class='fw-actions'>"
            "<button type=\"button\" id=\"fwCheckUpdates\" class=\"fw-check-btn\" "
            "data-fw-version=\"" + String(VERSION) + "\" "
            "data-api-latest=\"" + String(FIRMWARE_REPO_RELEASES_LATEST_API_URL) + "\" "
            "data-api-main-h=\"" + String(FIRMWARE_REPO_MAIN_H_CONTENTS_API_URL) + "\" "
            "data-default-branch=\"" + String(FIRMWARE_REPO_DEFAULT_BRANCH) + "\" "
            "data-releases=\"" + String(FIRMWARE_REPO_RELEASES_URL) + "\" "
            "data-branch-url=\"" + String(FIRMWARE_REPO_BRANCH_URL) + "\">Check for updates</button>"
            "<span class='gh-sponsor-embed'><iframe src=\"" + String(FIRMWARE_SPONSOR_BUTTON_SRC) + "\" title=\"Sponsor on GitHub\" width=\"114\" height=\"32\" loading=\"lazy\" referrerpolicy=\"no-referrer-when-downgrade\"></iframe></span></div>"
            "<span id=\"fwUpdateResult\" class=\"fw-update-msg\" aria-live=\"polite\"></span></div>"
            "<div class='sub-card-row' style='margin-top:8px'><b>Firmware Build: </b><span>" + String(BUILD) + "</span></div>"
            "<div class='fw-repo-links' role='navigation' aria-label='Firmware repository'><a href=\"" + String(FIRMWARE_REPO_README_URL) + "\" target=\"_blank\" rel=\"noopener\">README</a>"
            "<span class='fw-repo-sep' aria-hidden='true'>&middot;</span><a href=\"" + String(FIRMWARE_REPO_RELEASES_URL) + "\" target=\"_blank\" rel=\"noopener\">Releases</a>"
            "<span class='fw-repo-sep' aria-hidden='true'>&middot;</span><a href=\"" + String(FIRMWARE_REPO_BRANCH_URL) + "\" target=\"_blank\" rel=\"noopener\">Source (" + String(FIRMWARE_REPO_DEFAULT_BRANCH) + ")</a></div></div>";
  }
  String release = String(__DATE__) + " - " + String(__TIME__);
  html += "<div class='sys-advanced-block advanced-only'>";
  html += "<div class='sub-card'><p class='sub-card-title'>Memory</p><div class='sys-stat-tiles'>"
          "<div class='sys-stat-tile'><span class='fw-compare-label'>Free Heap</span><span class='sys-stat-val'>" + formatNumberWithCommas(ESP.getFreeHeap()) + "</span></div>"
          "<div class='sys-stat-tile'><span class='fw-compare-label'>Free PSRAM</span><span class='sys-stat-val'>" + formatNumberWithCommas(ESP.getFreePsram()) + "</span></div>"
          "<div class='sys-stat-tile'><span class='fw-compare-label'>Free Stack</span><span class='sys-stat-val'>" + formatNumberWithCommas(uxTaskGetStackHighWaterMark(NULL)) + "</span></div>"
          "</div></div>";
  appendGatewayChipTempStateSubCard(html);
#ifdef LOCAL_CLIENT
  html += "<div class='sub-card'><p class='sub-card-title'>RS485 today</p>"
          "<div style='margin-bottom:8px'><span class='diag-badge' style='font-weight:700;color:#fff;background:" + String(rs485Stats.polarityInverted ? "#0f4a87" : "#4b5563") + "'>" + String(rs485Stats.polarityInverted ? "inverted_rx_tx" : "normal") + "</span></div>"
          "<dl class='wifi-kv'><dt>Valid frames</dt><dd>" + formatNumberWithCommas(rs485Stats.messagesToday) + "</dd>"
          "<dt>CRC errors</dt><dd>" + formatNumberWithCommas(rs485Stats.crcToday) + "</dd>"
          "<dt>Last frame age</dt><dd>";
  if (rs485Stats.lastValidFrameMs > 0)
  {
    html += formatNumberWithCommas(rs485LastValidAgeMs) + " ms";
  }
  else
  {
    html += "n/a";
  }
  html += "</dd></dl></div>";
#endif
  html += "<div class='sub-card'><p class='sub-card-title'>Build</p>"
          "<div class='sub-card-row'><b>Release: </b><span>" + release + "</span></div>"
          "<p class='sys-build-def'>" + buildDefinitionString + "</p></div>";
  html += "<div class='sub-card'><p class='sub-card-title'>Gateway Actions</p>"
          "<p style='margin:0 0 8px 0;font-size:14px;color:var(--muted)'>Restart the ESP32 gateway. Spa control and telemetry pause briefly during reboot.</p>"
          "<button type=\"button\" id=\"gwRebootBtn\" class=\"fw-check-btn fw-danger-btn\">Reboot gateway</button>"
          "<span id=\"gwRebootResult\" class=\"fw-update-msg\" aria-live=\"polite\"></span></div>";
  html += "</div></section>";

  appendWifiStateSection(html);
  html += "<section class='panel'><h1>MQTT</h1><ul>";
  html += "<li><b>Status: </b>" + String(mqtt.connected() ? "Connected" : "Disconnected");
  html += " (" + String(mqtt.state()) + ", " + String(mqttStateName(mqtt.state())) + ")</li>";
  html += "<li><b>Broker: </b>" + String(MQTT_SERVER) + ":" + String(MQTT_PORT) + "</li>";
  html += "<li><b>Topic Root: </b>" + mqttTopic + "</li>";
  html += "<li><b>Command Topic: </b>" + mqttTopic + "cmd/#</li>";
  html += "<li><b>LWT Topic: </b>" + mqttTopic + "node/state</li>";
  html += "<li><b>HA Discovery: </b>" + String(MQTT_HA_DISCOVERY ? "enabled" : "disabled") +
          " (prefix: " + String(MQTT_DISCOVERY_PREFIX) + ", temp unit: " + String(MQTT_HA_TEMP_UNIT) + ")</li>";
  html += "<li class='advanced-only'><b>MQTT State Legend: </b>0=Connected, -1=Disconnected, -2=Connect failed, -3=Connection lost, -4=Timeout, -5=Bad credentials</li>";
  html += "<li><details><summary><b>MQTT credentials and configuration note</b></summary>"
          "<p style='margin:8px 0 0 0'>Credentials are hidden in the portal by design. Update <code>MQTT_SERVER</code>, <code>MQTT_PORT</code>, <code>BROKER_LOGIN</code>, and <code>BROKER_PASS</code> in <code>src/config.h</code>, then rebuild/reflash firmware.</p>"
          "<p style='margin:8px 0 0 0'>Other MQTT behavior is configured in <code>src/config.h</code> via <code>MQTT_HA_DISCOVERY</code>, <code>MQTT_DISCOVERY_PREFIX</code>, and <code>MQTT_HA_TEMP_UNIT</code>.</p>"
          "</details></li>";
  html += "<li class='advanced-only'><b>Spa status struct magic (ESP RAM): </b>" + String(spaStatusData.magicNumber) +
          " <span style=\"font-size:12px;color:var(--muted)\">(expected 0x12345678 after init; not from spa controller)</span></li>";

  html += "</ul></section><section class='panel'><h1>Spa Data Freshness</h1>";
  html += "<p style='margin:0 0 8px 0;font-size:14px;color:var(--muted)'>Status Snapshot is the live spa status frame stream used for controls/telemetry. Dataset rows below are separate config/info blocks with independent freshness and retry behavior.</p>";
  html += "<table class='state-freshness'><thead><tr><th>Dataset</th><th>Last Update</th><th>Stale</th><th>Retry</th></tr></thead><tbody>";
  html += "<tr><td>Status Snapshot</td><td>" + statusLastUpdateDisplayHtml(spaStatusData.lastUpdate) + "</td><td>" + String(staleData(spaStatusData) ? "yes" : "no") + "</td><td>n/a</td></tr>";
  html += "<tr><td>Configuration</td><td>" + statusLastUpdateDisplayHtml(spaConfigurationData.lastUpdate) + "</td><td>" + String(staleData(spaConfigurationData) ? "yes" : "no") + "</td><td>" + String(retryRequest(spaConfigurationData) ? "yes" : "no") + "</td></tr>";
  html += "<tr><td>Preferences</td><td>" + statusLastUpdateDisplayHtml(spaPreferencesData.lastUpdate) + "</td><td>" + String(staleData(spaPreferencesData) ? "yes" : "no") + "</td><td>" + String(retryRequest(spaPreferencesData) ? "yes" : "no") + "</td></tr>";
  html += "<tr><td>Filters</td><td>" + statusLastUpdateDisplayHtml(spaFilterSettingsData.lastUpdate) + "</td><td>" + String(staleData(spaFilterSettingsData) ? "yes" : "no") + "</td><td>" + String(retryRequest(spaFilterSettingsData) ? "yes" : "no") + "</td></tr>";
  html += "<tr><td>Information</td><td>" + statusLastUpdateDisplayHtml(spaInformationData.lastUpdate) + "</td><td>" + String(staleData(spaInformationData) ? "yes" : "no") + "</td><td>" + String(retryRequest(spaInformationData) ? "yes" : "no") + "</td></tr>";
  html += "<tr><td>Fault</td><td>" + statusLastUpdateDisplayHtml(spaFaultLogData.lastUpdate) + "</td><td>" + String(staleData(spaFaultLogData) ? "yes" : "no") + "</td><td>" + String(retryRequest(spaFaultLogData) ? "yes" : "no") + "</td></tr>";
  html += "<tr><td>spaSettings0x04Data</td><td>" + statusLastUpdateDisplayHtml(spaSettings0x04Data.lastUpdate) + "</td><td>" + String(staleData(spaSettings0x04Data) ? "yes" : "no") + "</td><td>" + String(retryRequest(spaSettings0x04Data) ? "yes" : "no") + "</td></tr>";
  html += "</tbody></table></section>";

  html += "<section class='panel'><h1 id='api-shortcuts'>API Shortcuts</h1><p style='margin:0 0 8px 0;font-size:14px;color:var(--muted)'>Raw/history endpoints can be noisy and large.</p><ul>";
  html += "<li><b>Live Wi-Fi snapshot: </b><a href='/api/wifi' target='_blank' rel='noopener'>GET /api/wifi</a></li>";
  html += "<li><b>Live MQTT snapshot: </b><a href='/api/mqtt' target='_blank' rel='noopener'>GET /api/mqtt</a></li>";
  html += "<li><b>Firmware metadata: </b><a href='/api/version' target='_blank' rel='noopener'>GET /api/version</a></li>";
  html += "<li><b>Gateway diagnostics: </b><a href='/api/diagnostics' target='_blank' rel='noopener'>GET /api/diagnostics</a></li>";
  html += "<li><b>RS485 summary diagnostics: </b><a href='/api/rs485' target='_blank' rel='noopener'>GET /api/rs485</a></li>";
  html += "<li><b>RS485 retry UART (safe mode): </b><code>POST /api/rs485/retry</code></li>";
  html += "<li><b>RS485 raw byte trace: </b><a href='/api/rs485/raw?limit=200' target='_blank' rel='noopener'>GET /api/rs485/raw?limit=200</a></li>";
  html += "<li><b>RS485 history snapshots: </b><a href='/api/rs485/history?limit=200' target='_blank' rel='noopener'>GET /api/rs485/history?limit=200</a></li>";
  html += "</ul></section>";
  html += "<section class='panel'><h1>LittleFS Inventory</h1><p style='margin:0 0 8px 0;font-size:14px;color:var(--muted)'>Load on demand when needed for debugging.</p>"
          "<button type='button' id='stateLoadLittleFs' class='fw-check-btn'>Load LittleFS files</button>"
          "<ul id='stateLittleFsBox' style='margin-top:10px'></ul></section>";

  html += "<section class='panel advanced-panel'><h1>Advanced Diagnostics</h1>";
  html += "<div class='sub-card'><p class='sub-card-title'>Gateway fault log</p>"
          "<p style='margin:0 0 8px 0;font-size:14px;color:var(--muted)'>RTC diagnostic ring from <code>GET /api/diagnostics</code> (survives panic reboot; not the spa pack fault log on <a href='/config#cfg-history'>Config</a>).</p>"
          "<div class='sub-card-row'><b>Device uptime: </b><span id='gwFaultUptime'>&mdash;</span></div>"
          "<pre id='gwFaultLogBox' class='config-hex' style='max-height:220px;overflow:auto;margin-top:8px'>Loading&hellip;</pre></div>";

#ifdef LOCAL_CLIENT
  html += "<p class='rs485-hint'>" + rs485HealthHint(rsHealth) + "</p>";
  html += "<p class='rs485-hint'>" + rs485ModeHint(rs485Stats.polarityInverted) + "</p>";
  html += "<p class='rs485-deep-meta'>Detect phase " + String(rs485Stats.polarityLocked ? "2 (locked)" : (rs485Stats.polarityInverted ? "1 (testing inverted_rx_tx)" : "0 (testing normal)"));
  html += "<span class='wifi-meta__sep'>&middot;</span>Polarity locked " + String(rs485Stats.polarityLocked ? "yes" : "no");
  html += "<span class='wifi-meta__sep'>&middot;</span>AUTO_TX " + String(rs485AutoTxEnabled() ? "true" : "false");
  html += "<span class='wifi-meta__sep'>&middot;</span>Health code " + rsHealth + "</p>";
  html += "<table class='state-freshness'><thead><tr><th>Metric</th><th>Today</th><th>Yesterday</th></tr></thead><tbody>";
  html += "<tr><td>Raw bytes</td><td>" + formatNumberWithCommas(rs485Stats.rawBytesToday) + "</td><td>" + formatNumberWithCommas(rs485Stats.rawBytesYesterday) + "</td></tr>";
  html += "<tr><td>Raw bytes (normal)</td><td>" + formatNumberWithCommas(rs485Stats.rawBytesNormalToday) + "</td><td>" + formatNumberWithCommas(rs485Stats.rawBytesNormalYesterday) + "</td></tr>";
  html += "<tr><td>Raw bytes (inverted)</td><td>" + formatNumberWithCommas(rs485Stats.rawBytesInvertedToday) + "</td><td>" + formatNumberWithCommas(rs485Stats.rawBytesInvertedYesterday) + "</td></tr>";
  html += "<tr><td>Frame attempts</td><td>" + formatNumberWithCommas(rs485Stats.framesToday) + "</td><td>" + formatNumberWithCommas(rs485Stats.framesYesterday) + "</td></tr>";
  html += "<tr><td>0x7E markers</td><td>" + formatNumberWithCommas(rs485Stats.frameMarkersToday) + "</td><td>" + formatNumberWithCommas(rs485Stats.frameMarkersYesterday) + "</td></tr>";
  html += "<tr><td>Valid frames</td><td>" + formatNumberWithCommas(rs485Stats.messagesToday) + "</td><td>" + formatNumberWithCommas(rs485Stats.messagesYesterday) + "</td></tr>";
  html += "<tr><td>CRC errors</td><td>" + formatNumberWithCommas(rs485Stats.crcToday) + "</td><td>" + formatNumberWithCommas(rs485Stats.crcYesterday) + "</td></tr>";
  html += "<tr><td>Format errors</td><td>" + formatNumberWithCommas(rs485Stats.badFormatToday) + "</td><td>" + formatNumberWithCommas(rs485Stats.badFormatYesterday) + "</td></tr>";
  html += "<tr><td>Mode switches</td><td>" + formatNumberWithCommas(rs485Stats.polaritySwitchesToday) + "</td><td>" + formatNumberWithCommas(rs485Stats.polaritySwitchesYesterday) + "</td></tr>";
  html += "<tr><td>Max UART backlog</td><td>" + formatNumberWithCommas(rs485Stats.maxUartAvailableToday) + "</td><td>" + formatNumberWithCommas(rs485Stats.maxUartAvailableYesterday) + "</td></tr>";
  html += "</tbody></table>";
  html += "<dl class='wifi-kv' style='margin-top:10px'><dt>UART</dt><dd>RX GPIO " + String(rs485RxGpio()) + ", TX GPIO " + String(rs485TxGpio()) + ", " + String(rs485Baud()) + " baud</dd>";
  html += "<dt>Polarity inverted</dt><dd>" + String(rs485Stats.polarityInverted ? "true" : "false") + "</dd></dl>";
#endif
  html += "</section></div><script>(function(){var t=document.getElementById('toggleAdvanced');if(!t)return;t.addEventListener('change',function(){document.body.classList.toggle('show-advanced',t.checked);});})();</script>";
  html += "<script>(function(){var up=document.getElementById('gwFaultUptime');var box=document.getElementById('gwFaultLogBox');if(!up||!box)return;"
          "fetch('/api/diagnostics',{cache:'no-store'}).then(function(r){if(!r.ok)throw new Error('http');return r.json();}).then(function(j){"
          "var ms=j&&typeof j.deviceUptimeMs==='number'?j.deviceUptimeMs:null;"
          "up.textContent=ms===null?'\\u2014':(Math.floor(ms/1000).toLocaleString()+' s');"
          "var arr=j&&j.faultLog?j.faultLog:[];if(!arr.length){box.textContent='(empty)';return;}"
          "var lines=[];for(var i=0;i<arr.length;i++){var e=arr[i]||{};var when=e.wallTime||(typeof e.uptimeMs==='number'?('uptime '+e.uptimeMs+' ms'):'');"
          "lines.push((when?when+' \\u2014 ':'')+String(e.msg||''));}"
          "box.textContent=lines.join('\\n');"
          "}).catch(function(){box.textContent='Failed to load /api/diagnostics';});})();</script>";
  html += "<script>(function(){var btn=document.getElementById('stateLoadLittleFs');var box=document.getElementById('stateLittleFsBox');if(!btn||!box)return;"
          "btn.addEventListener('click',function(){if(btn.disabled)return;btn.disabled=true;btn.textContent='Loading...';"
          "fetch('/api/state/littlefs',{cache:'no-store'}).then(function(r){if(!r.ok)throw new Error('http');return r.json();}).then(function(j){"
          "box.innerHTML='<li>'+(j&&j.html?j.html:'(empty)')+'</li>';btn.textContent='LittleFS loaded';}).catch(function(){btn.disabled=false;btn.textContent='Retry LittleFS load';});});})();</script>";
  html += "<script>(function(){var btn=document.getElementById('fwCheckUpdates');if(!btn)return;var el=document.getElementById('fwUpdateResult');var cur=document.getElementById('fwCurrentBadge');var relBadge=document.getElementById('fwLatestReleaseBadge');var brBadge=document.getElementById('fwLatestBranchBadge');var apiLatest=btn.getAttribute('data-api-latest');var apiMainH=btn.getAttribute('data-api-main-h');var releases=btn.getAttribute('data-releases');var branchUrl=btn.getAttribute('data-branch-url');var branch=btn.getAttribute('data-default-branch')||'branch';var fw=btn.getAttribute('data-fw-version');var ghHdr={Accept:'application/vnd.github+json'};function norm(s){return String(s||'').trim().replace(/^v/i,'');}function dispTag(s){s=String(s||'').trim();if(!s)return'\u2014';return/^v/i.test(s)?s:('v'+s);}function cmpSemver(a,b){var pa=norm(a).split('.').map(function(x){return parseInt(x,10)||0;});var pb=norm(b).split('.').map(function(x){return parseInt(x,10)||0;});var n=Math.max(pa.length,pb.length,3);for(var i=0;i<n;i++){var da=(pa[i]||0),db=(pb[i]||0);if(da<db)return-1;if(da>db)return 1;}return 0;}function setMsg(state,text){var colors={idle:'var(--muted)',checking:'#b26a00',ok:'#1b5e20',warn:'#b00020',error:'#6b7280'};el.style.color=colors[state]||colors.idle;el.textContent=text;}function renderMsg(state,parts){var colors={idle:'var(--muted)',checking:'#b26a00',ok:'#1b5e20',warn:'#b00020',error:'#6b7280'};el.style.color=colors[state]||colors.idle;el.textContent='';for(var i=0;i<(parts||[]).length;i++){var p=parts[i];if(p.href){var a=document.createElement('a');a.href=p.href;a.textContent=p.t;a.target='_blank';a.rel='noopener';el.appendChild(a);}else{el.appendChild(document.createTextNode(p.t||''));}}}function parseVersionFromMainH(text){var m=String(text||'').match(/#define\\s+VERSION\\s+\"([^\"]+)\"/);return m?m[1]:'';}function fetchRelease(){return fetch(apiLatest,{headers:ghHdr}).then(function(r){if(!r.ok)throw new Error('http');return r.json();}).then(function(j){return j.tag_name||'';});}function fetchBranch(){return fetch(apiMainH,{headers:ghHdr}).then(function(r){if(!r.ok)throw new Error('http');return r.json();}).then(function(j){if(!j||j.encoding!=='base64'||!j.content)return '';var text=atob(String(j.content).replace(/\\n/g,''));return parseVersionFromMainH(text);});}function showManualLink(){renderMsg('error',[{t:'Could not reach GitHub. '},{t:'Open Releases',href:releases},{t:' or '},{t:'Source ('+branch+')',href:branchUrl},{t:' to compare manually.'}]);}if(cur)cur.textContent=dispTag(fw);btn.addEventListener('click',function(){setMsg('checking','Checking GitHub...');if(relBadge)relBadge.textContent='\u2014';if(brBadge)brBadge.textContent='\u2014';Promise.allSettled([fetchRelease(),fetchBranch()]).then(function(results){var relOk=results[0].status==='fulfilled';var brOk=results[1].status==='fulfilled';var relVer=relOk?results[0].value:'';var brVer=brOk?results[1].value:'';if(relBadge)relBadge.textContent=relOk?dispTag(relVer):'\u2014';if(brBadge)brBadge.textContent=brOk?dispTag(brVer):'\u2014';if(!relOk&&!brOk){showManualLink();return;}var behindRel=relOk&&cmpSemver(fw,relVer)<0;var behindBr=brOk&&cmpSemver(fw,brVer)<0;if(!behindRel&&!behindBr){setMsg('ok',(!relOk||!brOk)?'Up to date (partial check).':'Up to date.');return;}var parts=[];if(behindRel&&behindBr){if(cmpSemver(relVer,brVer)===0){parts=[{t:'Newer tagged release available ('+dispTag(relVer)+') \u2014 see '},{t:'Releases',href:releases},{t:' for changelog / tag.'}];}else{parts=[{t:'Newer tagged release ('+dispTag(relVer)+') and '+branch+' tip ('+dispTag(brVer)+') available \u2014 see '},{t:'Releases',href:releases},{t:' and '},{t:'Source ('+branch+')',href:branchUrl},{t:'.'}];}}else if(behindRel){parts=[{t:'Newer tagged release available ('+dispTag(relVer)+') \u2014 see '},{t:'Releases',href:releases},{t:' for changelog / tag.'}];}else{parts=[{t:'Newer on '+branch+' branch ('+dispTag(brVer)+') \u2014 not tagged yet; see '},{t:'Source ('+branch+')',href:branchUrl},{t:' to build from tip.'}];}if(!relOk||!brOk){parts.push({t:' Some GitHub data unavailable.'});}renderMsg('warn',parts);});});})();</script>";
  html += "<script>(function(){var btn=document.getElementById('gwRebootBtn');var el=document.getElementById('gwRebootResult');if(!btn||!el)return;"
          "btn.addEventListener('click',function(){"
          "if(!confirm('Reboot this gateway? Spa control and telemetry will be unavailable briefly.'))return;"
          "btn.disabled=true;el.style.color='#b26a00';el.textContent='Rebooting...';"
          "fetch('/restart',{cache:'no-store'}).catch(function(){});"
          "setTimeout(function(){el.style.color='var(--muted)';el.textContent='Gateway is rebooting. Reload this page in ~30 seconds.';},500);"
          "});})();</script></main></div></body></html>";

  String etag = String("W/\"state-") + String(VERSION) + "-" + String(BUILD) + "-" + String(spaStatusData.lastUpdate) + "-" + String(spaConfigurationData.lastUpdate) + "\"";
  logPortalHtmlMissingGlobalCss(sawPortalCss, html.length(), "/state");
  Log.verbose(F("[Web]: /state assembled len=%u slabs=%u freeHeap=%u maxAlloc=%u healthy=%u" CR),
              static_cast<unsigned>(html.length()), static_cast<unsigned>(html.slabCount()),
              static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()),
              html.healthy() ? 1u : 0u);
  sendHtmlChunksWithEtag(request, html, etag);
  Log.verbose("[Web]: handleState %p %s %s" CR, request->client()->remoteIP(), request->methodToString(), request->url().c_str());

  // Log.verbose(F("[Web]: Response sent %s" CR), html.c_str());
}

void handleLogsApi(AsyncWebServerRequest *request)
{
  uint32_t since = 0;
  if (request->hasParam("since"))
  {
    since = (uint32_t)request->getParam("since")->value().toInt();
  }
  unsigned limit = 120;
  if (request->hasParam("limit"))
  {
    limit = (unsigned)request->getParam("limit")->value().toInt();
  }
  String body;
  webLogBufferBuildJsonSince(since, limit, Log.getLevel(), body);
  request->send(200, "application/json", body);
}

void handleLogsConfigGet(AsyncWebServerRequest *request)
{
  String body;
  webLogBufferBuildJsonLogConfig(Log.getLevel(), body);
  request->send(200, "application/json", body);
}

void handleLogsConfigPost(AsyncWebServerRequest *request)
{
  if (request->_tempObject == nullptr)
  {
    request->send(400, "application/json", "{\"error\":\"no_body\"}");
    return;
  }
  String *bodyPtr = (String *)request->_tempObject;
  String body = *bodyPtr;
  delete bodyPtr;
  request->_tempObject = nullptr;

  DynamicJsonDocument doc(256);
  DeserializationError err = deserializeJson(doc, body);
  if (err)
  {
    request->send(400, "application/json", "{\"error\":\"bad_json\"}");
    return;
  }
  if (!doc.containsKey("level"))
  {
    request->send(400, "application/json", "{\"error\":\"missing_level\"}");
    return;
  }
  int level = doc["level"].as<int>();
  if (level < LOG_LEVEL_SILENT)
  {
    level = LOG_LEVEL_SILENT;
  }
  if (level > LOG_LEVEL)
  {
    level = LOG_LEVEL;
  }
  Log.setLevel(level);
  String reply = "{\"ok\":true,\"level\":";
  reply += String(level);
  reply += "}";
  request->send(200, "application/json", reply);
}

void handleLogsPage(AsyncWebServerRequest *request)
{
  if (!portalLargePageTryBegin(request, "/logs"))
  {
    return;
  }
  String html;
  html.reserve(40000);
  html = F("<html class=\"logs-portal\">");
  const bool sawPortalCss = appendPortalHead(html, "Spa Logs", ",viewport-fit=cover", portalLogsHeadExtraStyle);
  html += F("<body class=\"logs-portal\"><a class='skip-link' href='#mainContent'>Skip to main content</a><div class='page logs-page'>");
  html += webMenuLogs;
  html += F("<main id='mainContent'><section class='panel logs-panel'><div class='logs-stack'><h1>Device logs</h1>");
  html += "<p style='color:var(--muted);font-size:14px;margin-top:0'>Recent lines are buffered on the gateway; include/exclude filters run in the browser. Logs are teed to USB <code>Serial</code> (monitor baud) and this ring. For a live tail without USB, use this page or <code>GET /api/logs</code> (optional WebSocket tail). If the firmware was built with <code>TELNET_LOG</code>, <code>TelnetStream</code> also listens on TCP port 23; the global logger is <em>not</em> switched to Telnet (see Wi‑Fi boot messages).</p>";
  html += R"HTML(<style>
html.logs-portal,body.logs-portal{min-height:100svh;min-height:100dvh}
body.logs-portal{display:flex;flex-direction:column;margin:0;box-sizing:border-box;padding-bottom:env(safe-area-inset-bottom,0)}
body.logs-portal>.page.logs-page{flex:1 1 auto;display:flex;flex-direction:column;min-height:0;width:100%;max-width:980px;margin:0 auto;padding:max(var(--space-3),env(safe-area-inset-left,0)) max(var(--space-3),env(safe-area-inset-right,0)) max(var(--space-3),env(safe-area-inset-bottom,0))}
body.logs-portal #mainContent{flex:1 1 auto;display:flex;flex-direction:column;min-height:0}
body.logs-portal .logs-panel{flex:1 1 auto;display:flex;flex-direction:column;min-height:0;margin-bottom:0}
body.logs-portal .logs-stack{flex:0 0 auto}
.log-controls{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-bottom:10px}
.log-controls input[type=text]{flex:1 1 140px;min-width:120px;padding:8px;border:1px solid var(--border);border-radius:6px;font-size:14px}
.log-controls label{font-size:14px;color:var(--muted)}
.log-controls select{padding:8px;border-radius:6px;border:1px solid var(--border);font-size:14px}
.preset-row{display:flex;gap:8px;flex-wrap:wrap;margin:0 0 10px 0}
.preset-row button{flex:0 0 auto;padding:8px 11px;font-size:13px;min-height:36px}
.status-row{display:flex;align-items:center;gap:10px;margin:0 0 10px 0;color:var(--muted);font-size:13px}
.log-view{flex:1 1 12rem;min-height:0;overflow:auto;background:#0f172a;color:#e2e8f0;font-family:ui-monospace,Menlo,Consolas,monospace;font-size:12px;line-height:1.45;padding:8px;border-radius:8px;border:1px solid var(--border)}
.log-line{display:flex;gap:8px;padding:2px 4px;border-radius:4px;white-space:pre-wrap;word-break:break-word}
.log-seq{color:#93a8c5;min-width:56px}
.log-tag{display:inline-block;padding:0 6px;border-radius:999px;background:#233148;color:#d7e3f4;font-size:11px}
.lvl-e{background:rgba(190,24,36,.2)} .lvl-w{background:rgba(202,138,4,.2)} .lvl-i{background:rgba(2,132,199,.16)} .lvl-v{background:rgba(71,85,105,.2)}
#newBadge{display:none}
@media (max-width:640px){body.logs-portal>.page.logs-page{padding-left:max(var(--space-2),env(safe-area-inset-left,0));padding-right:max(var(--space-2),env(safe-area-inset-right,0))}}
</style>)HTML";
  html += "<div class='preset-row'><button type='button' id='pAll'>All</button><button type='button' id='pErr'>Errors only</button><button type='button' id='pRs'>RS485</button><button type='button' id='pBridge'>BridgeDiag</button><button type='button' id='pWifi'>WiFi</button></div>";
  html += "<div class='log-controls'><label>Level <select id='lvl'><option value='0'>SILENT</option><option value='1'>FATAL</option><option value='2'>ERROR</option><option value='3'>WARNING</option><option value='4'>INFO/NOTICE</option><option value='5'>TRACE</option><option value='6'>VERBOSE</option></select></label>";
  html += "<button type='button' id='applyLvl'>Apply level</button>";
  html += "<label>Include <input type='text' id='fInc' placeholder='substring' autocapitalize='off' autocomplete='off'/></label>";
  html += "<label>Exclude <input type='text' id='fExc' placeholder='substring' autocapitalize='off' autocomplete='off'/></label>";
  html += "<label><input type='checkbox' id='pause'/> Pause</label>";
  html += "<label><input type='checkbox' id='hideIdleCts' checked/> Hide idle CTS</label>";
  html += "<label><input type='checkbox' id='showHidden'/> Show hidden</label>";
  html += "<label><input type='checkbox' id='useWs' checked/> WebSocket tail</label>";
  html += "<button type='button' id='newBadge'>0 new lines</button>";
  html += "<button type='button' id='clr'>Clear view</button><button type='button' id='copyTxt'>Copy</button><button type='button' id='dlTxt'>Download .log</button><button type='button' id='dlJson'>Download .json</button>";
  html += "</div>";
  html += "<div class='status-row'><span id='streamMode'>poll</span><span id='renderCount'>0 lines</span><span id='hiddenCount'>hidden idle CTS: 0</span><span id='connState'></span></div></div>";
  html += "<div id='logView' class='log-view' aria-live='polite'></div></section></main></div><script>";
  html += R"JS((function(){
var logView=document.getElementById('logView'),since=0,pollMs=1000,pollMaxMs=20000,timer,ws,useWs=true,newBuffered=0;
var pollFailures=0,wsRetryTimer=null,wsOpenEver=false,pollInFlight=null;
var fInc=document.getElementById('fInc'),fExc=document.getElementById('fExc'),sel=document.getElementById('lvl');
var pauseEl=document.getElementById('pause'),newBadge=document.getElementById('newBadge');
var hideIdleCtsEl=document.getElementById('hideIdleCts'),showHiddenEl=document.getElementById('showHidden');
var streamMode=document.getElementById('streamMode'),renderCount=document.getElementById('renderCount'),hiddenCountEl=document.getElementById('hiddenCount'),connState=document.getElementById('connState');
var rendered=[],maxRendered=8000;
var hiddenIdleCts=0;
function abortLogPoll(){if(pollInFlight){pollInFlight.abort();pollInFlight=null;}}
function getTag(t){var m=t.match(/\[([^\]]+)\]/);return m?m[1]:'';}
function getLevelClass(t){if(/\bE:|\bERROR\b/.test(t))return'lvl-e';if(/\bW:|\bWARNING\b/.test(t))return'lvl-w';if(/\bI:|\bNOTICE\b|\bINFO\b/.test(t))return'lvl-i';if(/\bTRACE\b|\bVERBOSE\b/.test(t))return'lvl-v';return'';}
function esc(s){return s.replace(/[&<>"]/g,function(c){return({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'})[c];});}
function isIdleCtsLine(t){
var l=t.toLowerCase();
if(l.indexOf('[bridgediag]')<0)return false;
if(l.indexOf('cts')<0)return false;
if(l.indexOf('depth_before=0')<0)return false;
return true;
}
function passes(t){var i=(fInc.value||'').trim();var x=(fExc.value||'').trim();if(i&&t.toLowerCase().indexOf(i.toLowerCase())<0)return false;if(x&&t.toLowerCase().indexOf(x.toLowerCase())>=0)return false;return true;}
function isVisibleRecord(rec){
var line=rec.t;
var hiddenByIdleCts=hideIdleCtsEl.checked&&isIdleCtsLine(line);
if(hiddenByIdleCts&&!showHiddenEl.checked)return false;
if(!passes(line))return false;
return true;
}
function renderLine(rec){var tag=getTag(rec.t),cls=getLevelClass(rec.t);var body=esc(rec.t);if(tag){body=body.replace('['+tag+']','<span class=\"log-tag\">['+esc(tag)+']</span>');}
return '<div class=\"log-line '+cls+'\"><span class=\"log-seq\">#'+rec.s+'</span><span>'+body+'</span></div>';}
function refreshFromRendered(){var out='',n=0,h=0;for(var i=0;i<rendered.length;i++){var line=rendered[i].t;var hiddenByIdleCts=hideIdleCtsEl.checked&&isIdleCtsLine(line);if(hiddenByIdleCts){h++;}if(!isVisibleRecord(rendered[i]))continue;out+=renderLine(rendered[i]);n++;}hiddenIdleCts=h;hiddenCountEl.textContent='hidden idle CTS: '+String(hiddenIdleCts);var stick=(logView.scrollTop+logView.clientHeight+20)>=logView.scrollHeight;logView.innerHTML=out;renderCount.textContent=n+' lines';if(stick){logView.scrollTop=logView.scrollHeight;}}
function appendLines(arr){if(!arr)return;for(var j=0;j<arr.length;j++){rendered.push({s:arr[j].s,t:arr[j].t});if(rendered.length>maxRendered)rendered.shift();}refreshFromRendered();}
function receiveLines(arr){if(!arr||!arr.length)return;var atBottom=(logView.scrollTop+logView.clientHeight+20)>=logView.scrollHeight;
if(atBottom){appendLines(arr);newBuffered=0;newBadge.style.display='none';}
else{newBuffered+=arr.length;newBadge.textContent=String(newBuffered)+' new lines';newBadge.style.display='inline-flex';for(var k=0;k<arr.length;k++){rendered.push({s:arr[k].s,t:arr[k].t});if(rendered.length>maxRendered)rendered.shift();}renderCount.textContent=rendered.length+' lines';}}
function capSel(mx){for(var i=0;i<sel.options.length;i++){var o=sel.options[i];o.disabled=(parseInt(o.value,10)>mx);}if((parseInt(sel.value,10)||0)>mx)sel.value=String(mx);}
function nextPollDelay(){var e=Math.min(pollMaxMs,pollMs*Math.pow(2,Math.min(6,pollFailures)));var j=Math.floor(Math.random()*Math.max(250,Math.floor(e*0.35)));return Math.min(pollMaxMs,e+j);}
function schedulePoll(ms){stopPoll();timer=setTimeout(poll,Math.max(250,ms||pollMs));}
function fetchJsonTimeout(url,timeoutMs){var ctl=new AbortController();var t=setTimeout(function(){ctl.abort();},timeoutMs||5000);return fetch(url,{cache:'no-store',signal:ctl.signal}).then(function(r){if(!r.ok)throw new Error('http_'+r.status);return r.json();}).finally(function(){clearTimeout(t);});}
function poll(){if(document.hidden||pauseEl.checked)return;var ctl=new AbortController();pollInFlight=ctl;var t=setTimeout(function(){ctl.abort();},4200);schedulePoll(pollMs);
fetch('/api/logs?since='+since+'&limit=120',{cache:'no-store',signal:ctl.signal}).then(function(r){if(!r.ok)throw new Error('http_'+r.status);return r.json();}).then(function(j){if(pauseEl.checked||pollInFlight!==ctl)return;pollFailures=0;connState.textContent='ok';
if(typeof j.compileMaxLevel==='number')capSel(j.compileMaxLevel);
var lines=j.lines||[];
receiveLines(lines);
if(lines.length>0&&typeof lines[lines.length-1].s==='number'){since=lines[lines.length-1].s;}
else if(typeof j.newestSeq==='number'){since=j.newestSeq;}
}).catch(function(){if(pauseEl.checked||pollInFlight!==ctl)return;pollFailures++;connState.textContent='poll retrying...';schedulePoll(nextPollDelay());}).finally(function(){clearTimeout(t);if(pollInFlight===ctl)pollInFlight=null;});}
function startPoll(){stopPoll();streamMode.textContent='poll';pollFailures=0;poll();}
function stopPoll(){if(timer){clearTimeout(timer);timer=null;}}
function clearWsRetry(){if(wsRetryTimer){clearTimeout(wsRetryTimer);wsRetryTimer=null;}}
function scheduleWsReconnect(){clearWsRetry();if(document.hidden||pauseEl.checked||!useWs)return;var wait=Math.min(20000,1000*Math.pow(2,Math.min(6,pollFailures)));wsRetryTimer=setTimeout(connectWs,wait);}
function connectWs(){if(document.hidden||pauseEl.checked||!useWs)return;streamMode.textContent='ws';clearWsRetry();var p=location.protocol==='https:'?'wss:':'ws:';ws=new WebSocket(p+'//'+location.host+'/api/logs/ws');connState.textContent='connecting';
ws.onopen=function(){pollFailures=0;wsOpenEver=true;connState.textContent='ws-open';};
ws.onmessage=function(ev){if(pauseEl.checked)return;try{var o=JSON.parse(ev.data);if(o.lines)receiveLines(o.lines);if(o.d)receiveLines(o.d);}catch(e){}};
ws.onerror=function(){connState.textContent='ws-error';};
ws.onclose=function(){ws=null;pollFailures++;if(!useWs)return;connState.textContent='ws-closed';if(wsOpenEver&&!pauseEl.checked){startPoll();}scheduleWsReconnect();};}
function setPreset(inc,exc){fInc.value=inc||'';fExc.value=exc||'';refreshFromRendered();}
function dl(name,content,type){var b=new Blob([content],{type:type});var a=document.createElement('a');a.href=URL.createObjectURL(b);a.download=name;document.body.appendChild(a);a.click();setTimeout(function(){URL.revokeObjectURL(a.href);a.remove();},0);}
document.getElementById('pAll').addEventListener('click',function(){setPreset('','');});
document.getElementById('pErr').addEventListener('click',function(){setPreset('E:','');});
document.getElementById('pRs').addEventListener('click',function(){setPreset('[RS485]','');});
document.getElementById('pBridge').addEventListener('click',function(){setPreset('[BridgeDiag]','');});
document.getElementById('pWifi').addEventListener('click',function(){setPreset('[WiFi]','');});
fInc.addEventListener('input',refreshFromRendered);fExc.addEventListener('input',refreshFromRendered);
hideIdleCtsEl.addEventListener('change',refreshFromRendered);
showHiddenEl.addEventListener('change',refreshFromRendered);
newBadge.addEventListener('click',function(){newBuffered=0;newBadge.style.display='none';refreshFromRendered();logView.scrollTop=logView.scrollHeight;});
document.getElementById('pause').addEventListener('change',function(){if(this.checked){stopPoll();clearWsRetry();abortLogPoll();if(ws){ws.close();ws=null;}}else if(useWs)connectWs();else startPoll();});
document.getElementById('useWs').addEventListener('change',function(){useWs=this.checked;stopPoll();clearWsRetry();abortLogPoll();if(ws){ws.close();ws=null;}if(!pauseEl.checked){if(useWs)connectWs();else startPoll();}});
document.getElementById('clr').addEventListener('click',function(){rendered=[];refreshFromRendered();});
document.getElementById('copyTxt').addEventListener('click',function(){
var txt='';for(var i=0;i<rendered.length;i++){if(isVisibleRecord(rendered[i]))txt+=rendered[i].t+'\n';}
if(!txt){connState.textContent='nothing to copy';return;}
if(navigator.clipboard&&navigator.clipboard.writeText){
navigator.clipboard.writeText(txt).then(function(){connState.textContent='copied';}).catch(function(){fallbackCopy(txt);});
}else{fallbackCopy(txt);}
});
function fallbackCopy(txt){
var ta=document.createElement('textarea');ta.value=txt;ta.setAttribute('readonly','readonly');
ta.style.position='fixed';ta.style.top='-1000px';document.body.appendChild(ta);ta.focus();ta.select();
try{var ok=document.execCommand('copy');connState.textContent=ok?'copied':'copy failed';}
catch(e){connState.textContent='copy failed';}
document.body.removeChild(ta);
}
document.getElementById('dlTxt').addEventListener('click',function(){var txt='';for(var i=0;i<rendered.length;i++){if(isVisibleRecord(rendered[i]))txt+=rendered[i].t+'\n';}dl('spa-logs-'+Date.now()+'.log',txt,'text/plain');});
document.getElementById('dlJson').addEventListener('click',function(){var out=[];for(var i=0;i<rendered.length;i++){if(isVisibleRecord(rendered[i]))out.push(rendered[i]);}dl('spa-logs-'+Date.now()+'.json',JSON.stringify(out,null,2),'application/json');});
document.getElementById('applyLvl').addEventListener('click',function(){var v=parseInt(sel.value,10);fetch('/api/logs/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({level:v})}).then(function(){return fetchJsonTimeout('/api/logs/config',5000);}).then(function(c){if(typeof c.currentLevel==='number')sel.value=String(c.currentLevel);if(typeof c.compileMaxLevel==='number')capSel(c.compileMaxLevel);}).catch(function(){});});
fetchJsonTimeout('/api/logs/config',5000).then(function(c){sel.value=String(c.currentLevel||0);capSel(c.compileMaxLevel||6);}).catch(function(){});
document.addEventListener('visibilitychange',function(){if(document.hidden){stopPoll();clearWsRetry();abortLogPoll();if(ws){ws.close();ws=null;}}else if(!pauseEl.checked){if(useWs)connectWs();else startPoll();}});
if(!pauseEl.checked){if(useWs)connectWs();else startPoll();}
})();)JS";
  html += "</script></body></html>";
  String etag = String("W/\"logs-") + String(VERSION) + "-" + String(BUILD) + "\"";
  logPortalHtmlMissingGlobalCss(sawPortalCss, html.length(), "/logs");
  sendHtmlWithEtag(request, html, etag, true);
  Log.verbose("[Web]: handleLogsPage %p" CR, request->client()->remoteIP());
}

void handleVersion(AsyncWebServerRequest *request)
{
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(1024);
  doc["version"] = VERSION;
  doc["build"] = BUILD;
  doc["hostname"] = WiFi.getHostname();
  doc["ip"] = WiFi.localIP().toString();
  doc["espResetReason"] = getEspResetReasonText();
  // Composite first: on non-SW boots this clears a stale soft label before lastRestartIntent is read.
  doc["restartReason"] = getLastRestartReason();
  doc["lastRestartIntent"] = getLastRestartIntent();
  doc["repoReadmeUrl"] = FIRMWARE_REPO_README_URL;
  doc["releasesUrl"] = FIRMWARE_REPO_RELEASES_URL;
  doc["branchUrl"] = FIRMWARE_REPO_BRANCH_URL;
  doc["releasesLatestApiUrl"] = FIRMWARE_REPO_RELEASES_LATEST_API_URL;
  doc["defaultBranch"] = FIRMWARE_REPO_DEFAULT_BRANCH;
  doc["mainHContentsApiUrl"] = FIRMWARE_REPO_MAIN_H_CONTENTS_API_URL;
  serializeJson(doc, *response);
  request->send(response);
}

void handleDiagnostics(AsyncWebServerRequest *request)
{
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(10240);
  // Use to<> once — a second to<JsonObject>() clears the document (ArduinoJson 6).
  JsonObject root = doc.to<JsonObject>();
  faultCaptureAppendToJson(root);
  appendGatewayChipTempJson(doc);
  spaPanelClockAutoSyncAppendToJson(root);
  root["version"] = VERSION;
  root["hostname"] = WiFi.getHostname();
  root["ip"] = WiFi.localIP().toString();
  root["otaRunningPartition"] = otaRunningPartitionLabel();
  root["otaPartitionState"] = otaRunningPartitionState();
  root["otaBootVerified"] = otaBootVerifiedThisRun();
#ifdef LOCAL_CLIENT
  root["uartBegun"] = rs485UartBegun();
  root["rs485SafeMode"] = rs485SafeModeActive();
  root["safeModeReason"] = rs485SafeModeReason();
  root["rs485FaultBootStreak"] = rs485FaultBootStreak();
  root["rs485BeginAttempted"] = rs485BeginAttemptedFlag();
  root["retryPending"] = rs485RetryPending();
  root["rxGpio"] = rs485RxGpio();
  root["txGpio"] = rs485TxGpio();
  root["rs485Health"] = rs485HealthCode();
#endif
  serializeJson(doc, *response);
  request->send(response);
}

void handleWifi(AsyncWebServerRequest *request)
{
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(1024);
  wl_status_t st = WiFi.status();
  const bool ok = (st == WL_CONNECTED);
  doc["connected"] = ok;
  doc["status"] = static_cast<int>(st);
  doc["statusName"] = wifiStatusName(st);
  doc["mac"] = WiFi.macAddress();
  if (WiFi.getHostname())
  {
    doc["hostname"] = WiFi.getHostname();
  }
  else
  {
    doc["hostname"] = "";
  }
  if (wifiBssidLockActive() && wifiConfiguredBssidLock()[0] != '\0')
  {
    doc["bssidLock"] = wifiConfiguredBssidLock();
  }
  else
  {
    doc["bssidLock"] = "";
  }
  doc["lastDisconnectReason"] = wifiLastDisconnectReasonCode();
  doc["connectAttempts"] = wifiConnectAttemptCount();
  if (ok)
  {
    doc["ssid"] = WiFi.SSID();
    doc["rssi"] = WiFi.RSSI();
    doc["ip"] = WiFi.localIP().toString();
    doc["gateway"] = WiFi.gatewayIP().toString();
    doc["subnet"] = WiFi.subnetMask().toString();
    doc["dns"] = WiFi.dnsIP(0).toString();
    doc["channel"] = WiFi.channel();
    doc["bssid"] = WiFi.BSSIDstr();
  }
  else
  {
    doc["ssid"] = "";
    doc["ip"] = "";
    doc["gateway"] = "";
    doc["subnet"] = "";
    doc["dns"] = "";
    doc["channel"] = 0;
    doc["bssid"] = "";
  }
  serializeJson(doc, *response);
  request->send(response);
}

void handleMqtt(AsyncWebServerRequest *request)
{
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(1024);
  const int stateCode = mqtt.state();
  const bool connected = mqtt.connected();
  doc["enabled"] = true;
  doc["connected"] = connected;
  doc["stateCode"] = stateCode;
  doc["stateName"] = mqttStateName(stateCode);
  doc["brokerHost"] = MQTT_SERVER;
  doc["brokerPort"] = MQTT_PORT;
  doc["topicRoot"] = mqttTopic;
  doc["commandTopic"] = mqttTopic + "cmd/#";
  doc["lwtTopic"] = mqttTopic + "node/state";
  doc["haDiscoveryEnabled"] = MQTT_HA_DISCOVERY ? true : false;
  doc["haDiscoveryPrefix"] = MQTT_DISCOVERY_PREFIX;
  doc["haTempUnit"] = MQTT_HA_TEMP_UNIT;
  doc["reconnectIntervalMs"] = 30000;
  doc["lastReconnectAttemptMs"] = mqttLastReconnectAttempt;
  if (mqttLastReconnectAttempt > 0 && millis() >= mqttLastReconnectAttempt)
  {
    doc["lastReconnectAttemptMsAgo"] = millis() - mqttLastReconnectAttempt;
  }
  else
  {
    doc["lastReconnectAttemptMsAgo"] = -1;
  }
  doc["credentialsExposed"] = false;
  doc["credentialNote"] = "Update MQTT_SERVER/MQTT_PORT/BROKER_LOGIN/BROKER_PASS in src/config.h and reflash.";
  serializeJson(doc, *response);
  request->send(response);
}

static void fillStatusSnapshotDoc(DynamicJsonDocument &doc)
{
  doc["lastUpdate"] = spaStatusData.lastUpdate;
  doc["snapshotAgeSec"] = statusSnapshotAgeSec();
  doc["snapshotAtLocal"] = statusLastUpdateDisplayHtml(spaStatusData.lastUpdate);
  doc["snapshotMeta"] = statusSnapshotSubtitle();
  doc["tempScaleCelsius"] = spaStatusData.tempScale ? true : false;
  doc["tempRange"] = spaStatusData.tempRange;
  doc["spaState"] = spaStatusData.spaState;
  doc["spaStateText"] = getMapDescription(spaStatusData.spaState, spaStateMap);
  doc["initMode"] = spaStatusData.initMode;
  doc["initModeText"] = getMapDescription(spaStatusData.initMode, initModeMap);
  doc["heatingMode"] = spaStatusData.heatingMode;
  doc["heatingModeText"] = getMapDescription(spaStatusData.heatingMode, heatingModeMap);
  doc["heatingState"] = spaStatusData.heatingState;
  doc["heatingStateText"] = getMapDescription(spaStatusData.heatingState, heatingStateMap);
  doc["needsHeat"] = spaStatusData.needsHeat ? 1 : 0;
  doc["highSetTemp"] = spaStatusData.highSetTemp;
  doc["lowSetTemp"] = spaStatusData.lowSetTemp;
  doc["setTemp"] = spaStatusData.setTemp;
  {
    float bandMin = 0;
    float bandMax = 0;
    spaProtocolActiveSetpointBand(bandMin, bandMax);
    doc["setTempMin"] = bandMin;
    doc["setTempMax"] = bandMax;
  }
  doc["light1"] = spaStatusData.light1 ? 1 : 0;
  doc["light2"] = spaStatusData.light2 ? 1 : 0;
  doc["pump1"] = spaStatusData.pump1;
  doc["pump2"] = spaStatusData.pump2;
  doc["pump3"] = spaStatusData.pump3;
  doc["pump4"] = spaStatusData.pump4;
  doc["pump5"] = spaStatusData.pump5;
  doc["pump6"] = spaStatusData.pump6;
  doc["pump1Config"] = statusPumpConfigSpeed(1);
  doc["pump2Config"] = statusPumpConfigSpeed(2);
  doc["pump3Config"] = statusPumpConfigSpeed(3);
  doc["pump4Config"] = statusPumpConfigSpeed(4);
  doc["pump5Config"] = statusPumpConfigSpeed(5);
  doc["pump6Config"] = statusPumpConfigSpeed(6);
  doc["pump1On"] = statusPumpIsOn(1);
  doc["pump2On"] = statusPumpIsOn(2);
  doc["pump3On"] = statusPumpIsOn(3);
  doc["pump4On"] = statusPumpIsOn(4);
  doc["pump5On"] = statusPumpIsOn(5);
  doc["pump6On"] = statusPumpIsOn(6);
  doc["circ"] = spaStatusData.circ ? 1 : 0;
  doc["blower"] = spaStatusData.blower;
  doc["mister"] = spaStatusData.mister ? 1 : 0;
  doc["panelTime"] = String(spaStatusData.time);
  doc["clockFormat"] = statusPanelClockFormatLabel(spaStatusData.clockMode);
  doc["clockModeRaw"] = spaStatusData.clockMode;
  doc["filterModeText"] = String(getMapDescription(spaStatusData.filterMode, filterModeMap));
  doc["filter1_running"] = spaFilter1Running() ? 1 : 0;
  doc["filter2_running"] = spaFilter2Running() ? 1 : 0;
  doc["reminderType"] = spaStatusData.reminderType;
  doc["reminderText"] =
      spaReminderText(spaStatusData.reminderType, spaStatusData.spaState);
  doc["reminderHint"] =
      spaReminderHintText(spaStatusData.reminderType, spaStatusData.spaState);
  doc["reminderActive"] = spaReminderIsActive(
      spaStatusData.reminderType, spaStatusData.spaState, spaStatusData.initMode);
  doc["reminderIsFault"] = spaReminderIsFault(spaStatusData.reminderType);
  doc["gatewayTimeHHMM"] = statusGatewayLocalTimeHHMM();
}

void handleStatusControlsApi(AsyncWebServerRequest *request)
{
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(2560);
  fillStatusSnapshotDoc(doc);
  serializeJson(doc, *response);
  request->send(response);
}

void handleStatusSummaryApi(AsyncWebServerRequest *request)
{
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(2560);
  fillStatusSnapshotDoc(doc);
  wl_status_t st = WiFi.status();
  doc["wifiConnected"] = (st == WL_CONNECTED);
  doc["wifiStatus"] = static_cast<int>(st);
  doc["wifiStatusName"] = wifiStatusName(st);
  if (st == WL_CONNECTED)
  {
    doc["wifiRssi"] = WiFi.RSSI();
  }
  serializeJson(doc, *response);
  request->send(response);
}

void handleStatusHistoriesApi(AsyncWebServerRequest *request)
{
  if (!spaStatusData.heatOn || !spaStatusData.filterOn)
  {
    request->send(503, "application/json", "{\"error\":\"analytics_not_ready\"}");
    return;
  }

  DynamicJsonDocument doc(28672);
  String historiesHtml;
  historiesHtml.reserve(9000);
  appendStatusHistoriesSection(historiesHtml);
  doc["html"] = historiesHtml;
  doc["tempIsCelsius"] = (statusSpaTempReady() && spaStatusData.tempScale) ? 1 : 0;
  doc["tempSlotCount"] = TEMP_HISTORY_SLOTS;
  doc["tempSampleMinutes"] = 10;
  doc["heatIncludesToday"] = 1;
  doc["filterIncludesToday"] = 1;

  JsonArray tempHistory = doc.createNestedArray("tempHistory");
  for (int i = TEMP_HISTORY_SLOTS - 1; i >= 0; i--)
  {
    tempHistory.add(tempHistoryData.samples[i]);
  }

  const unsigned long heatTodaySec = spaStatusData.heatOn->today();
  const unsigned long filterTodaySec = spaStatusData.filterOn->today();
  doc["heatTodaySeconds"] = heatTodaySec;
  doc["filterTodaySeconds"] = filterTodaySec;

  JsonArray heatSeconds = doc.createNestedArray("heatSeconds");
  appendMergedDailySecondsHistory(heatSeconds, spaStatusData.heatOn->history(), heatTodaySec);
  JsonArray filterSeconds = doc.createNestedArray("filterSeconds");
  appendMergedDailySecondsHistory(filterSeconds, spaStatusData.filterOn->history(), filterTodaySec);

  const size_t jsonNeed = measureJson(doc);
  if (jsonNeed == 0 || jsonNeed > doc.capacity())
  {
    Log.error("[Web]: /api/status/histories JSON overflow need=%u cap=%u" CR,
              static_cast<unsigned>(jsonNeed), static_cast<unsigned>(doc.capacity()));
    request->send(500, "application/json", "{\"error\":\"json_overflow\"}");
    return;
  }

  String payload;
  payload.reserve(jsonNeed + 1);
  if (serializeJson(doc, payload) == 0)
  {
    Log.error("[Web]: /api/status/histories serialize failed need=%u" CR, static_cast<unsigned>(jsonNeed));
    request->send(500, "application/json", "{\"error\":\"serialize_failed\"}");
    return;
  }

  request->send(200, "application/json", payload);
}

void handleStateLittleFsApi(AsyncWebServerRequest *request)
{
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(6144);
  doc["html"] = listDirToString(LittleFS, "/", 3);
  serializeJson(doc, *response);
  request->send(response);
}

void handleDiagToggleApi(AsyncWebServerRequest *request)
{
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(768);

  if (!request->hasParam("item"))
  {
    doc["ok"] = false;
    doc["error"] = "missing item query parameter";
    serializeJson(doc, *response);
    request->send(response);
    return;
  }

  const String itemStr = request->getParam("item")->value();
  int itemCode = itemStr.toInt();
  if (itemCode <= 0 || itemCode > 255)
  {
    doc["ok"] = false;
    doc["error"] = "invalid item query parameter";
    doc["item"] = itemStr;
    serializeJson(doc, *response);
    request->send(response);
    return;
  }

  String dest = "wifi";
  if (request->hasParam("dest"))
  {
    dest = request->getParam("dest")->value();
    dest.toLowerCase();
  }
  bool useWifiDestination = (dest != "id");

  bool includeZeroPad = true;
  if (request->hasParam("pad"))
  {
    String pad = request->getParam("pad")->value();
    pad.toLowerCase();
    includeZeroPad = !(pad == "none" || pad == "0");
  }

  String frameHex;
  SpaCommandResult result = spaSendToggleDiagnostic(
      (uint8_t)itemCode,
      useWifiDestination,
      includeZeroPad,
      SPA_COMMAND_SOURCE_WEB,
      &frameHex);

  doc["ok"] = result.accepted;
  doc["item"] = itemCode;
  doc["dest"] = useWifiDestination ? "wifi" : "id";
  doc["pad"] = includeZeroPad ? "00" : "none";
  doc["result"] = result.reason;
  doc["frame"] = frameHex;
  JsonObject snapshot = doc.createNestedObject("snapshot");
  fillPumpDiagSnapshot(snapshot);

  serializeJson(doc, *response);
  request->send(response);
}

void handleDiagToggleSequenceApi(AsyncWebServerRequest *request)
{
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(4096);

  if (!request->hasParam("item"))
  {
    doc["ok"] = false;
    doc["error"] = "missing item query parameter";
    serializeJson(doc, *response);
    request->send(response);
    return;
  }

  const String itemStr = request->getParam("item")->value();
  int itemCode = itemStr.toInt();
  if (itemCode <= 0 || itemCode > 255)
  {
    doc["ok"] = false;
    doc["error"] = "invalid item query parameter";
    doc["item"] = itemStr;
    serializeJson(doc, *response);
    request->send(response);
    return;
  }

  int repeats = 1;
  if (request->hasParam("repeats"))
  {
    repeats = request->getParam("repeats")->value().toInt();
  }
  repeats = constrain(repeats, 1, 6);

  int gapMs = 1200;
  if (request->hasParam("gap_ms"))
  {
    gapMs = request->getParam("gap_ms")->value().toInt();
  }
  gapMs = constrain(gapMs, 200, 10000);

  int observeMs = 5000;
  if (request->hasParam("observe_ms"))
  {
    observeMs = request->getParam("observe_ms")->value().toInt();
  }
  observeMs = constrain(observeMs, 0, 20000);

  String dest = "wifi";
  if (request->hasParam("dest"))
  {
    dest = request->getParam("dest")->value();
    dest.toLowerCase();
  }
  bool useWifiDestination = (dest != "id");

  bool includeZeroPad = true;
  if (request->hasParam("pad"))
  {
    String pad = request->getParam("pad")->value();
    pad.toLowerCase();
    includeZeroPad = !(pad == "none" || pad == "0");
  }

  doc["item"] = itemCode;
  doc["dest"] = useWifiDestination ? "wifi" : "id";
  doc["pad"] = includeZeroPad ? "00" : "none";
  doc["repeats"] = repeats;
  doc["gapMs"] = gapMs;
  doc["observeMs"] = observeMs;

  JsonObject before = doc.createNestedObject("before");
  fillPumpDiagSnapshot(before);

  JsonArray attempts = doc.createNestedArray("attempts");
  bool acceptedAny = false;
  bool rejectedAny = false;
  const unsigned long startMs = millis();

  for (int i = 0; i < repeats; i++)
  {
    String frameHex;
    SpaCommandResult result = spaSendToggleDiagnostic(
        (uint8_t)itemCode,
        useWifiDestination,
        includeZeroPad,
        SPA_COMMAND_SOURCE_WEB,
        &frameHex);

    JsonObject attempt = attempts.createNestedObject();
    attempt["index"] = i + 1;
    attempt["accepted"] = result.accepted;
    attempt["reason"] = result.reason;
    attempt["frame"] = frameHex;
    attempt["elapsedMs"] = millis() - startMs;

    if (result.accepted)
    {
      acceptedAny = true;
    }
    else
    {
      rejectedAny = true;
      break;
    }

    if (i < repeats - 1)
    {
      delay(gapMs);
    }
  }

  if (observeMs > 0)
  {
    delay(observeMs);
  }

  JsonObject after = doc.createNestedObject("after");
  fillPumpDiagSnapshot(after);

  const bool pump1Changed = before["pump1"].as<int>() != after["pump1"].as<int>();
  const bool light1Changed = before["light1"].as<int>() != after["light1"].as<int>();

  doc["acceptedAny"] = acceptedAny;
  doc["allAccepted"] = acceptedAny && !rejectedAny && (attempts.size() == static_cast<size_t>(repeats));
  doc["pump1Changed"] = pump1Changed;
  doc["light1Changed"] = light1Changed;
  doc["ok"] = acceptedAny;

  serializeJson(doc, *response);
  request->send(response);
}

void handleDiagLight1NextCtsApi(AsyncWebServerRequest *request)
{
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(2048);

  int observeMs = 2500;
  if (request->hasParam("observe_ms"))
  {
    observeMs = request->getParam("observe_ms")->value().toInt();
  }
  observeMs = constrain(observeMs, 200, 12000);

  String dest = "wifi";
  if (request->hasParam("dest"))
  {
    dest = request->getParam("dest")->value();
    dest.toLowerCase();
  }
  bool useWifiDestination = (dest != "id");

  bool includeZeroPad = true;
  if (request->hasParam("pad"))
  {
    String pad = request->getParam("pad")->value();
    pad.toLowerCase();
    includeZeroPad = !(pad == "none" || pad == "0");
  }

  JsonObject before = doc.createNestedObject("before");
  fillPumpDiagSnapshot(before);
  before["ctsMs"] = rs485LastCtsMs();
  before["ctsCount"] = rs485CtsCount();
  before["armCount"] = rs485NextCtsArmCount();
  before["fireCount"] = rs485NextCtsFireCount();
  before["queueDepth"] = static_cast<unsigned int>(uxQueueMessagesWaiting(spaWriteQueue));

  String frameHex;
  uint32_t armCount = 0;
  SpaCommandResult result = spaSendToggleOnNextCtsDiagnostic(
      0x11,
      useWifiDestination,
      includeZeroPad,
      SPA_COMMAND_SOURCE_WEB,
      &frameHex,
      &armCount);

  doc["ok"] = result.accepted;
  doc["result"] = result.reason;
  doc["frame"] = frameHex;
  doc["dest"] = useWifiDestination ? "wifi" : "id";
  doc["pad"] = includeZeroPad ? "00" : "none";
  doc["observeMs"] = observeMs;
  doc["armedAt"] = armCount;

  if (result.accepted)
  {
    const unsigned long waitStart = millis();
    bool fired = false;
    while (millis() - waitStart < static_cast<unsigned long>(observeMs))
    {
      if (rs485NextCtsFireCount() >= armCount)
      {
        fired = true;
        break;
      }
      delay(20);
    }
    doc["fired"] = fired;
    doc["firedAt"] = rs485NextCtsFireCount();
    doc["waitElapsedMs"] = millis() - waitStart;
  }
  else
  {
    doc["fired"] = false;
    doc["firedAt"] = rs485NextCtsFireCount();
    doc["waitElapsedMs"] = 0;
  }

  JsonObject after = doc.createNestedObject("after");
  fillPumpDiagSnapshot(after);
  after["ctsMs"] = rs485LastCtsMs();
  after["ctsCount"] = rs485CtsCount();
  after["armCount"] = rs485NextCtsArmCount();
  after["fireCount"] = rs485NextCtsFireCount();
  after["queueDepth"] = static_cast<unsigned int>(uxQueueMessagesWaiting(spaWriteQueue));

  doc["light1Changed"] = before["light1"].as<int>() != after["light1"].as<int>();
  serializeJson(doc, *response);
  request->send(response);
}

void handleDiagLight1NextCtsWindowApi(AsyncWebServerRequest *request)
{
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(16384);

  int observeMs = 6000;
  if (request->hasParam("observe_ms"))
  {
    observeMs = request->getParam("observe_ms")->value().toInt();
  }
  observeMs = constrain(observeMs, 500, 20000);

  int sampleMs = 250;
  if (request->hasParam("sample_ms"))
  {
    sampleMs = request->getParam("sample_ms")->value().toInt();
  }
  sampleMs = constrain(sampleMs, 100, 2000);

  String dest = "wifi";
  if (request->hasParam("dest"))
  {
    dest = request->getParam("dest")->value();
    dest.toLowerCase();
  }
  bool useWifiDestination = (dest != "id");

  bool includeZeroPad = true;
  if (request->hasParam("pad"))
  {
    String pad = request->getParam("pad")->value();
    pad.toLowerCase();
    includeZeroPad = !(pad == "none" || pad == "0");
  }

  doc["dest"] = useWifiDestination ? "wifi" : "id";
  doc["pad"] = includeZeroPad ? "00" : "none";
  doc["observeMs"] = observeMs;
  doc["sampleMs"] = sampleMs;

  JsonObject before = doc.createNestedObject("before");
  fillPumpDiagSnapshot(before);
  before["ctsMs"] = rs485LastCtsMs();
  before["ctsCount"] = rs485CtsCount();
  before["armCount"] = rs485NextCtsArmCount();
  before["fireCount"] = rs485NextCtsFireCount();
  before["queueDepth"] = static_cast<unsigned int>(uxQueueMessagesWaiting(spaWriteQueue));

  String frameHex;
  uint32_t armCount = 0;
  SpaCommandResult result = spaSendToggleOnNextCtsDiagnostic(
      0x11,
      useWifiDestination,
      includeZeroPad,
      SPA_COMMAND_SOURCE_WEB,
      &frameHex,
      &armCount);
  doc["ok"] = result.accepted;
  doc["result"] = result.reason;
  doc["frame"] = frameHex;
  doc["armedAt"] = armCount;

  JsonArray samples = doc.createNestedArray("samples");
  const unsigned long startMs = millis();
  const int maxSamples = 120;
  bool fired = false;

  while ((millis() - startMs) < static_cast<unsigned long>(observeMs))
  {
    JsonObject s = samples.createNestedObject();
    s["tMs"] = millis() - startMs;
    s["ctsCount"] = rs485CtsCount();
    s["fireCount"] = rs485NextCtsFireCount();
    fillPumpDiagSnapshot(s);
    if (rs485NextCtsFireCount() >= armCount && result.accepted)
    {
      fired = true;
    }
    if (samples.size() >= static_cast<size_t>(maxSamples))
    {
      break;
    }
    delay(sampleMs);
  }

  doc["fired"] = fired;
  doc["firedAt"] = rs485NextCtsFireCount();
  doc["sampleCount"] = samples.size();

  JsonObject after = doc.createNestedObject("after");
  fillPumpDiagSnapshot(after);
  after["ctsMs"] = rs485LastCtsMs();
  after["ctsCount"] = rs485CtsCount();
  after["armCount"] = rs485NextCtsArmCount();
  after["fireCount"] = rs485NextCtsFireCount();
  after["queueDepth"] = static_cast<unsigned int>(uxQueueMessagesWaiting(spaWriteQueue));

  doc["light1Changed"] = before["light1"].as<int>() != after["light1"].as<int>();
  serializeJson(doc, *response);
  request->send(response);
}

void handleRs485(AsyncWebServerRequest *request)
{
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(2048);
  doc["rxGpio"] = rs485RxGpio();
  doc["txGpio"] = rs485TxGpio();
  doc["baud"] = rs485Baud();
  doc["autoTx"] = rs485AutoTxEnabled();
  doc["rawBytesToday"] = rs485Stats.rawBytesToday;
  doc["rawBytesYesterday"] = rs485Stats.rawBytesYesterday;
  doc["rawBytesNormalToday"] = rs485Stats.rawBytesNormalToday;
  doc["rawBytesNormalYesterday"] = rs485Stats.rawBytesNormalYesterday;
  doc["rawBytesInvertedToday"] = rs485Stats.rawBytesInvertedToday;
  doc["rawBytesInvertedYesterday"] = rs485Stats.rawBytesInvertedYesterday;
  doc["framesToday"] = rs485Stats.framesToday;
  doc["framesYesterday"] = rs485Stats.framesYesterday;
  doc["messagesToday"] = rs485Stats.messagesToday;
  doc["messagesYesterday"] = rs485Stats.messagesYesterday;
  doc["crcToday"] = rs485Stats.crcToday;
  doc["crcYesterday"] = rs485Stats.crcYesterday;
  doc["badFormatToday"] = rs485Stats.badFormatToday;
  doc["badFormatYesterday"] = rs485Stats.badFormatYesterday;
  doc["frameMarkersToday"] = rs485Stats.frameMarkersToday;
  doc["frameMarkersYesterday"] = rs485Stats.frameMarkersYesterday;
  doc["maxUartAvailableToday"] = rs485Stats.maxUartAvailableToday;
  doc["maxUartAvailableYesterday"] = rs485Stats.maxUartAvailableYesterday;
  doc["rawCaptureOverflowsToday"] = rs485Stats.rawCaptureOverflowsToday;
  doc["rawCaptureOverflowsYesterday"] = rs485Stats.rawCaptureOverflowsYesterday;
  doc["polaritySwitchesToday"] = rs485Stats.polaritySwitchesToday;
  doc["polaritySwitchesYesterday"] = rs485Stats.polaritySwitchesYesterday;
  doc["polarityInverted"] = rs485Stats.polarityInverted;
  doc["polarityLocked"] = rs485Stats.polarityLocked;
  doc["mode"] = rs485Stats.polarityInverted ? "inverted_rx_tx" : "normal";
  doc["detectPhase"] = rs485Stats.polarityLocked ? 2 : (rs485Stats.polarityInverted ? 1 : 0);
  doc["lastByteMs"] = rs485Stats.lastByteMs;
  doc["lastValidFrameMs"] = rs485Stats.lastValidFrameMs;
  doc["lastCtsMs"] = rs485LastCtsMs();
  doc["ctsCount"] = rs485CtsCount();
  doc["nextCtsArmCount"] = rs485NextCtsArmCount();
  doc["nextCtsFireCount"] = rs485NextCtsFireCount();
  doc["health"] = rs485HealthCode();
  doc["uartBegun"] = rs485UartBegun();
  doc["rs485SafeMode"] = rs485SafeModeActive();
  doc["safeModeReason"] = rs485SafeModeReason();
  doc["rs485FaultBootStreak"] = rs485FaultBootStreak();
  doc["rs485BeginAttempted"] = rs485BeginAttemptedFlag();
  doc["retryPending"] = rs485RetryPending();

  serializeJson(doc, *response);
  request->send(response);
}

void handleRs485Retry(AsyncWebServerRequest *request)
{
  rs485RequestRetry();
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(512);
  doc["ok"] = true;
  doc["retryPending"] = rs485RetryPending();
  doc["rs485SafeMode"] = rs485SafeModeActive();
  doc["message"] = "RS485 UART begin scheduled on main loop";
  serializeJson(doc, *response);
  request->send(response);
}

void handleRs485Raw(AsyncWebServerRequest *request)
{
  int limit = 80;
  if (request->hasArg("limit"))
  {
    const int requested = request->arg("limit").toInt();
    if (requested > 0)
    {
      limit = requested;
    }
  }
  if (limit > RS485_RAW_CAPTURE_SIZE)
  {
    limit = RS485_RAW_CAPTURE_SIZE;
  }

  Rs485RawByte bytes[RS485_RAW_CAPTURE_SIZE];
  const int count = rs485GetRawRecent(bytes, limit);

  AsyncResponseStream *response = request->beginResponseStream("application/json");
  response->print("{\"count\":");
  response->print(count);
  response->print(",\"limit\":");
  response->print(limit);
  response->print(",\"rxGpio\":");
  response->print(rs485RxGpio());
  response->print(",\"txGpio\":");
  response->print(rs485TxGpio());
  response->print(",\"baud\":");
  response->print(rs485Baud());
  response->print(",\"mode\":\"");
  response->print(rs485Stats.polarityInverted ? "inverted_rx_tx" : "normal");
  response->print("\",\"bytesHex\":\"");
  for (int i = 0; i < count; i++)
  {
    char b[4];
    snprintf(b, sizeof(b), "%02X", bytes[i].value);
    if (i > 0)
    {
      response->print(' ');
    }
    response->print(b);
  }
  response->print("\",\"items\":[");
  for (int i = 0; i < count; i++)
  {
    char row[176];
    snprintf(row, sizeof(row),
             "%s{\"tMs\":%lu,\"gapMs\":%u,\"byte\":\"%02X\",\"dec\":%u,\"mode\":\"%s\",\"uartAvailable\":%u}",
             i > 0 ? "," : "",
             static_cast<unsigned long>(bytes[i].tMs),
             bytes[i].gapMs,
             bytes[i].value,
             bytes[i].value,
             bytes[i].polarityInverted ? "inverted_rx_tx" : "normal",
             bytes[i].uartAvailable);
    response->print(row);
  }
  response->print("]}");
  request->send(response);
}

void handleRs485History(AsyncWebServerRequest *request)
{
  int limit = 20;
  if (request->hasArg("limit"))
  {
    const int requested = request->arg("limit").toInt();
    if (requested > 0)
    {
      limit = requested;
    }
  }
  if (limit > RS485_HISTORY_SIZE)
  {
    limit = RS485_HISTORY_SIZE;
  }

  Rs485Snapshot snapshots[RS485_HISTORY_SIZE];
  const int count = rs485GetHistoryNewestFirst(snapshots, limit);

  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(16384);
  doc["count"] = count;
  doc["limit"] = limit;
  JsonArray items = doc.createNestedArray("items");
  for (int i = 0; i < count; i++)
  {
    JsonObject row = items.createNestedObject();
    row["tMs"] = snapshots[i].tMs;
    row["health"] = snapshots[i].health;
    row["mode"] = snapshots[i].polarityInverted ? "inverted_rx_tx" : "normal";
    row["detectPhase"] = snapshots[i].detectPhase;
    row["rawBytesToday"] = snapshots[i].rawBytesToday;
    row["rawBytesNormalToday"] = snapshots[i].rawBytesNormalToday;
    row["rawBytesInvertedToday"] = snapshots[i].rawBytesInvertedToday;
    row["framesToday"] = snapshots[i].framesToday;
    row["messagesToday"] = snapshots[i].messagesToday;
    row["crcToday"] = snapshots[i].crcToday;
    row["badFormatToday"] = snapshots[i].badFormatToday;
    row["polaritySwitchesToday"] = snapshots[i].polaritySwitchesToday;
  }

  serializeJson(doc, *response);
  request->send(response);
}

/*

This is the balboa cloud emulation

*/

String encodeResponse(uint8_t rawData[BALBOA_MESSAGE_SIZE], uint8_t length)
{
  if (length)
  {
    unsigned char message[BALBOA_MESSAGE_SIZE];
    for (int i = 0; i < length - 2 && i < BALBOA_MESSAGE_SIZE; i++)
    {
      message[i] = (char)rawData[i + 1];
    }
    message[length - 2] = '\0';

    //  Log.verbose("Encode: %s\n", message);
    // Base64 encode the string
    unsigned char encoded[BALBOA_MESSAGE_SIZE * 2 + 1];
    int encodedLength = encode_base64(message, length - 2, encoded);
    encoded[encodedLength] = '\0';
    char encodedString[BALBOA_MESSAGE_SIZE * 2];
    strncpy((char *)encodedString, (char *)encoded, encodedLength);
    encodedString[encodedLength] = '\0';
    //  Log.verbose("Encoded: %s\n", encodedString);
    return String(encodedString);
  }
  else
  {
    return "";
  }
}

static String filterSciReadResponse()
{
  return "<device_request target_name='Filters'>" +
         encodeResponse(spaFilterSettingsData.rawData, spaFilterSettingsData.rawDataLength) +
         "</device_request>";
}

static bool decodeSciFilterBlob(const String &b64, uint8_t outEight[8], String *err)
{
  if (b64.length() == 0)
  {
    if (err)
    {
      *err = "empty_filter_payload";
    }
    return false;
  }
  unsigned char decoded[32];
  memset(decoded, 0, sizeof(decoded));
  const unsigned int decodedLen = decode_base64((unsigned char *)b64.c_str(), decoded);
  if (decodedLen < 12)
  {
    if (err)
    {
      *err = "invalid_filter_payload";
    }
    return false;
  }
  if (decodedLen >= 4 && !(decoded[0] == 13 && decoded[1] == 0x0A && decoded[2] == 0xBF && decoded[3] == 0x23))
  {
    Log.verbose(F("[Web]: SCI filter blob header unexpected; using bytes 4-11" CR));
  }
  for (int i = 0; i < 8; i++)
  {
    outEight[i] = decoded[4 + i];
  }
  return true;
}

void handleConfigFilterGet(AsyncWebServerRequest *request)
{
  DynamicJsonDocument doc(768);
  spaConfigAppendFilterGetJson(doc.to<JsonObject>());
  String body;
  serializeJson(doc, body);
  request->send(200, "application/json", body);
}

void handleConfigFilterPost(AsyncWebServerRequest *request)
{
  if (request->_tempObject == nullptr)
  {
    request->send(400, "application/json", "{\"accepted\":false,\"reason\":\"no_body\"}");
    return;
  }
  String *bodyPtr = (String *)request->_tempObject;
  String body = *bodyPtr;
  delete bodyPtr;
  request->_tempObject = nullptr;

  DynamicJsonDocument doc(768);
  if (deserializeJson(doc, body))
  {
    request->send(400, "application/json", "{\"accepted\":false,\"reason\":\"bad_json\"}");
    return;
  }

  SpaFilterCycleSettings settings{};
  const char *err = nullptr;
  if (!spaParseFilterCycleJson(doc.as<JsonObjectConst>(), settings, false, &err))
  {
    DynamicJsonDocument out(256);
    out["accepted"] = false;
    out["reason"] = err ? err : "invalid_filter_payload";
    String reply;
    serializeJson(out, reply);
    request->send(400, "application/json", reply);
    return;
  }

  SpaCommandResult result = spaSetFilterCycles(settings, SPA_COMMAND_SOURCE_WEB);
  DynamicJsonDocument out(256);
  out["accepted"] = result.accepted;
  out["reason"] = result.reason;
  String reply;
  serializeJson(out, reply);
  request->send(result.accepted ? 200 : 409, "application/json", reply);
}

void handleConfigPreferencesGet(AsyncWebServerRequest *request)
{
  DynamicJsonDocument doc(512);
  spaConfigAppendPreferencesGetJson(doc.to<JsonObject>());
  String body;
  serializeJson(doc, body);
  request->send(200, "application/json", body);
}

void handleConfigPreferencesPost(AsyncWebServerRequest *request)
{
  if (request->_tempObject == nullptr)
  {
    request->send(400, "application/json", "{\"accepted\":false,\"reason\":\"no_body\"}");
    return;
  }
  String *bodyPtr = (String *)request->_tempObject;
  String body = *bodyPtr;
  delete bodyPtr;
  request->_tempObject = nullptr;

  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, body))
  {
    request->send(400, "application/json", "{\"accepted\":false,\"reason\":\"bad_json\"}");
    return;
  }

  if (!doc.containsKey("reminders"))
  {
    request->send(400, "application/json", "{\"accepted\":false,\"reason\":\"missing_reminders\"}");
    return;
  }

  const bool enabled = doc["reminders"].as<int>() != 0;
  SpaCommandResult result = spaSetPanelReminders(enabled, SPA_COMMAND_SOURCE_WEB);
  if (result.accepted)
  {
    spaRequestPreferences();
  }

  DynamicJsonDocument out(256);
  out["accepted"] = result.accepted;
  out["reason"] = result.reason;
  String reply;
  serializeJson(out, reply);
  request->send(result.accepted ? 200 : 409, "application/json", reply);
}

void handleConfigFaultLogGet(AsyncWebServerRequest *request)
{
  DynamicJsonDocument doc(512);
  spaConfigAppendFaultLogGetJson(doc.to<JsonObject>());
  String body;
  serializeJson(doc, body);
  request->send(200, "application/json", body);
}

void handleConfigFaultLogHistoryGet(AsyncWebServerRequest *request)
{
  DynamicJsonDocument doc(4096);
  spaConfigAppendFaultLogHistoryGetJson(doc.to<JsonObject>());
  String body;
  serializeJson(doc, body);
  request->send(200, "application/json", body);
}

void handleConfigFaultLogHistoryPost(AsyncWebServerRequest *request)
{
  DynamicJsonDocument out(128);
  if (spaFaultLogHistoryIsActive())
  {
    out["accepted"] = false;
    out["reason"] = "history_scan_active";
  }
  else if (spaFaultLogData.lastUpdate == 0)
  {
    out["accepted"] = false;
    out["reason"] = "fault_log_not_ready";
  }
  else if (!spaFaultLogHistoryStart())
  {
    out["accepted"] = false;
    out["reason"] = "history_scan_failed";
  }
  else
  {
    out["accepted"] = true;
    out["reason"] = "started";
  }
  String reply;
  serializeJson(out, reply);
  request->send(out["accepted"].as<bool>() ? 200 : 409, "application/json", reply);
}

void handleConfigExport(AsyncWebServerRequest *request)
{
  DynamicJsonDocument doc(4096);
  spaConfigAppendExportJson(doc.to<JsonObject>());
  String body;
  serializeJson(doc, body);

  String hostname = WiFi.getHostname() ? String(WiFi.getHostname()) : String("spa");
  hostname.replace(" ", "-");
  const time_t now = getTime();
  struct tm tmUtc;
  char ts[32] = "export";
  if (gmtime_r(&now, &tmUtc) != nullptr)
  {
    strftime(ts, sizeof(ts), "%Y%m%d-%H%M", &tmUtc);
  }
  String filename = "spa-config-" + hostname + "-" + String(ts) + ".json";

  AsyncWebServerResponse *response = request->beginResponse(200, "application/json", body);
  response->addHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  request->send(response);
}

void handleConfigImportPost(AsyncWebServerRequest *request)
{
  if (request->_tempObject == nullptr)
  {
    request->send(400, "application/json", "{\"accepted\":false,\"error\":\"no_body\"}");
    return;
  }
  String *bodyPtr = (String *)request->_tempObject;
  String body = *bodyPtr;
  delete bodyPtr;
  request->_tempObject = nullptr;

  DynamicJsonDocument doc(8192);
  if (deserializeJson(doc, body))
  {
    request->send(400, "application/json", "{\"accepted\":false,\"error\":\"bad_json\"}");
    return;
  }

  const bool dryRun = doc["dryRun"] | false;
  const bool force = doc["force"] | false;

  DynamicJsonDocument report(4096);
  spaConfigImportFromJson(doc, report.to<JsonObject>(), dryRun, force);
  String reply;
  serializeJson(report, reply);
  request->send(200, "application/json", reply);
}

void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
{
  // Log.verbose("[Web]: handleBody Request %s %s %d received from %p" CR, request->methodToString(), request->url().c_str(), index, request->client()->remoteIP());

  if (index == 0)
  {
    request->_tempObject = new String();
    if (request->_tempObject == nullptr)
    {
      Log.error("[Web]: handleBody String allocation failed (total=%d, len=%d)" CR, total, len);
      return;
    }
  }

  if (request->_tempObject == nullptr)
  {
    return;
  }

  // Append each body chunk; this works for both known-length and chunked transfers.
  String *body = (String *)request->_tempObject;
  for (size_t i = 0; i < len; i++)
  {
    body->concat((char)data[i]);
  }
}

// <sci_request version="1.0"><file_system><targets><device id="00 11 22 33 44 55 66 77"/></targets><commands><get_file path="PanelUpdate.txt"/></commands></file_system></sci_request>
// <sci_request version="1.0"><file_system><targets><device id="00 11 22 33 44 55 66 77"/></targets><commands><get_file path="SystemInformation.txt"/></commands></file_system></sci_request>
// <sci_request version="1.0"><file_system cache="false"><targets><device id="00 11 22 33 44 55 66 77" /></targets><commands><get_file path="SetupParameters.txt" /></commands></file_system></sci_request>

// <sci_request version="1.0"><data_service><targets><device id="00 11 22 33 44 55 66 77"/></targets><requests><device_request target_name="Request">Filters</device_request></requests></data_service></sci_request>

// <sci_request version="1.0"><data_service><targets><device id="00 11 22 33 44 55 66 77"/></targets><requests><device_request target_name="TempUnits">F</device_request></requests></data_service></sci_request>

String parseBody(String body)
{
  String response = "";
  if (body.indexOf("PanelUpdate.txt") > 0)
  {
    response = encodeResponse(spaStatusData.rawData, spaStatusData.rawDataLength);
  }
  else if (body.indexOf("DeviceConfiguration.txt") > 0)
  {
    response = encodeResponse(spaConfigurationData.rawData, spaConfigurationData.rawDataLength);
  }
  else if (body.indexOf("SetupParameters.txt") > 0)
  {
    response = encodeResponse(spaSettings0x04Data.rawData, spaSettings0x04Data.rawDataLength);
  }
  else if (body.indexOf("SystemInformation.txt") > 0)
  {
    response = encodeResponse(spaInformationData.rawData, spaInformationData.rawDataLength);
  }
  else if (body.indexOf("device_request") > 0)
  {
    using namespace tinyxml2;
    XMLDocument xmlDocument;
    xmlDocument.Parse(body.c_str());
    tinyxml2::XMLElement *root = xmlDocument.FirstChildElement("sci_request");
    tinyxml2::XMLElement *dataService = (root ? root->FirstChildElement("data_service") : nullptr);
    tinyxml2::XMLElement *requests = (dataService ? dataService->FirstChildElement("requests") : nullptr);
    tinyxml2::XMLElement *deviceRequestElement = (requests ? requests->FirstChildElement("device_request") : nullptr);

    if (!deviceRequestElement)
    {
      response = "<device_request result='rejected' error='invalid_xml'></device_request>";
      return response;
    }

    const char *targetName = deviceRequestElement->Attribute("target_name");
    const char *deviceRequestValue = deviceRequestElement->GetText();
    String target = (targetName ? String(targetName) : "");
    String value = (deviceRequestValue ? String(deviceRequestValue) : "");
    value.trim();

    if (target == "Request" && value.equalsIgnoreCase("Filters"))
    {
      response = filterSciReadResponse();
    }
    else if (target == "Filters")
    {
      if (value.length() == 0)
      {
        response = filterSciReadResponse();
      }
      else
      {
        uint8_t payload[8];
        String decodeErr;
        if (!decodeSciFilterBlob(value, payload, &decodeErr))
        {
          response = "<device_request target_name='Filters' result='rejected' error='" + decodeErr + "'></device_request>";
        }
        else
        {
          SpaFilterCycleSettings settings{};
          if (!spaParseFilterCyclePayload(payload, settings))
          {
            response = "<device_request target_name='Filters' result='rejected' error='invalid_filter_payload'></device_request>";
          }
          else
          {
            SpaCommandResult result = spaSetFilterCycles(settings, SPA_COMMAND_SOURCE_WEB);
            if (result.accepted)
            {
              response = "<device_request target_name='Filters' result='accepted'></device_request>";
            }
            else
            {
              response = "<device_request target_name='Filters' result='rejected' error='" + String(result.reason) + "'></device_request>";
            }
            Log.verbose("[Web]: Filters write -> %s" CR, result.reason);
          }
        }
      }
    }
    else if (target == "Button")
    {
      int separator = value.indexOf(':');
      String itemCodeRaw = (separator > 0 ? value.substring(0, separator) : value);
      String desiredStateRaw = (separator > 0 ? value.substring(separator + 1) : "");
      bool requestHasState = separator > 0;
      bool desiredOn = desiredStateRaw.equalsIgnoreCase("on");
      int itemCode = itemCodeRaw.toInt();
      if (itemCode <= 0 || itemCode > 255)
      {
        response = "<device_request target_name='Button' result='rejected' error='invalid_button_payload'>" + value + "</device_request>";
        return response;
      }

      int togglesToSend = spaToggleCountForButtonRequest((uint8_t)itemCode, requestHasState, desiredOn);
      Log.verbose("[Web]: Button request raw=%s item=%d desired=%s toggles=%d" CR, value.c_str(), itemCode, (requestHasState ? desiredStateRaw.c_str() : "n/a"), togglesToSend);
      if (togglesToSend < 0)
      {
        response = "<device_request target_name='Button' result='rejected' error='invalid_button_payload'>" + value + "</device_request>";
      }
      else if (togglesToSend == 0)
      {
        response = "<device_request target_name='Button' result='accepted'>" + value + "</device_request>";
        Log.verbose("[Web]: Button request no-op; already in desired state" CR);
      }
      else
      {
        SpaCommandResult result = {false, SPA_COMMAND_INVALID_ARGUMENT, "unknown"};
        for (int i = 0; i < togglesToSend; i++)
        {
          result = spaSendToggleCommand((uint8_t)itemCode, SPA_COMMAND_SOURCE_WEB);
          if (!result.accepted)
          {
            break;
          }
        }

        if (result.accepted)
        {
          response = "<device_request target_name='Button' result='accepted'>" + value + "</device_request>";
        }
        else
        {
          response = "<device_request target_name='Button' result='rejected' error='" + String(result.reason) + "'>" + value + "</device_request>";
        }
        Log.verbose("[Web]: Button request %s -> %s" CR, value.c_str(), result.reason);
      }
    }
    else if (target == "SetTemp")
    {
      float requested = value.toFloat();
      if (requested <= 0.0f)
      {
        response = "<device_request target_name='SetTemp' result='rejected' error='invalid_temp_payload'>" + value + "</device_request>";
        return response;
      }

      SpaCommandResult result = spaSetTargetTemperature(requested, SPA_COMMAND_SOURCE_WEB);
      if (result.accepted)
      {
        response = "<device_request target_name='SetTemp' result='accepted'>" + value + "</device_request>";
      }
      else
      {
        response = "<device_request target_name='SetTemp' result='rejected' error='" + String(result.reason) + "'>" + value + "</device_request>";
      }
      Log.verbose("[Web]: SetTemp request %s -> %s" CR, value.c_str(), result.reason);
    }
    else if (target == "SystemTime")
    {
      value.trim();
      const int colon = value.indexOf(':');
      if (colon <= 0 || colon >= (int)value.length() - 1)
      {
        response = "<device_request target_name='SystemTime' result='rejected' error='invalid_time_payload'>" + value + "</device_request>";
        return response;
      }
      const int hour = value.substring(0, colon).toInt();
      const int minute = value.substring(colon + 1).toInt();
      if (hour < 0 || hour > 23 || minute < 0 || minute > 59)
      {
        response = "<device_request target_name='SystemTime' result='rejected' error='invalid_time_payload'>" + value + "</device_request>";
        return response;
      }

      SpaCommandResult result = spaSetSpaPanelClockTime((uint8_t)hour, (uint8_t)minute, SPA_COMMAND_SOURCE_WEB);
      if (result.accepted)
      {
        response = "<device_request target_name='SystemTime' result='accepted'>" + value + "</device_request>";
      }
      else
      {
        response = "<device_request target_name='SystemTime' result='rejected' error='" + String(result.reason) + "'>" + value + "</device_request>";
      }
      Log.verbose("[Web]: SystemTime request %s -> %s" CR, value.c_str(), result.reason);
    }
    else if (target == "TimeFormat")
    {
      value.trim();
      bool parsed = false;
      bool use24 = false;
      if (value == "24")
      {
        parsed = true;
        use24 = true;
      }
      else if (value == "12")
      {
        parsed = true;
        use24 = false;
      }

      if (!parsed)
      {
        response = "<device_request target_name='TimeFormat' result='rejected' error='invalid_time_format_payload'>" + value + "</device_request>";
        return response;
      }

      SpaCommandResult result = spaSetSpaPanelClockFormat(use24, SPA_COMMAND_SOURCE_WEB);
      if (result.accepted)
      {
        response = "<device_request target_name='TimeFormat' result='accepted'>" + value + "</device_request>";
      }
      else
      {
        response = "<device_request target_name='TimeFormat' result='rejected' error='" + String(result.reason) + "'>" + value + "</device_request>";
      }
      Log.verbose("[Web]: TimeFormat request %s -> %s" CR, value.c_str(), result.reason);
    }
    else if (target == "TempUnits")
    {
      value.trim();
      bool parsed = false;
      bool celsius = false;
      if (value.equalsIgnoreCase("C") || value.equalsIgnoreCase("Celsius"))
      {
        parsed = true;
        celsius = true;
      }
      else if (value.equalsIgnoreCase("F") || value.equalsIgnoreCase("Fahrenheit"))
      {
        parsed = true;
        celsius = false;
      }

      if (!parsed)
      {
        response = "<device_request target_name='TempUnits' result='rejected' error='invalid_temp_units_payload'>" + value + "</device_request>";
        return response;
      }

      SpaCommandResult result = spaSetTemperatureScale(celsius, SPA_COMMAND_SOURCE_WEB);
      if (result.accepted)
      {
        response = "<device_request target_name='TempUnits' result='accepted'>" + value + "</device_request>";
      }
      else
      {
        response = "<device_request target_name='TempUnits' result='rejected' error='" + String(result.reason) + "'>" + value + "</device_request>";
      }
      Log.verbose("[Web]: TempUnits request %s -> %s" CR, value.c_str(), result.reason);
    }
    else
    {
      response = "<device_request target_name='" + target + "' result='rejected' error='unsupported_target'>" + value + "</device_request>";
      Log.verbose("[Web]: Unsupported device_request target=%s value=%s" CR, target.c_str(), value.c_str());
    }
  }
  else
  {
    // Log.verbose("[Web]: Error Unknown object requested %s" CR, body.c_str());
  }
  return response;
}

void handleData(AsyncWebServerRequest *request)
{
  // Log.verbose("[Web]: handleData Request %s %s received from %p" CR, request->methodToString(), request->url().c_str(), request->client()->remoteIP());

  if (request->_tempObject != nullptr)
  {
    // Log.verbose("[Web]: handleData _tempObject %s" CR, request->_tempObject);
    String *bodyPtr = (String *)request->_tempObject;
    String body = *bodyPtr;
    // Log.verbose("[Web]: handleData 1" CR);
    delete bodyPtr;
    // Log.verbose("[Web]: handleData 2" CR);
    request->_tempObject = nullptr;
    // Log.verbose("[Web]: handleData body %s" CR, body.c_str());

    String response = parseBody(body);
    if (response.length() == 0)
    {
      Log.verbose("[Web]: handleData no spa data yet for %s" CR, body.c_str());
      // Keep API responses explicit while avoiding noisy 404 loops during bench testing.
      request->send(200, "text/xml", "<response><ready>false</ready><error>no_spa_data_yet</error></response>");
      return;
    }
    // Log.verbose("[Web]: handleData response %s" CR, response.c_str());
    Log.verbose("[Web]: handleData %p %s %s %s" CR, request->client()->remoteIP(), request->methodToString(), request->url().c_str(), response.c_str());
    request->send(200, "text/xml", "<response><data>" + response + "</data></response>");
  }
  else
  {
    Log.verbose("[Web]: handleData no body" CR);
    request->send(200, "text/xml", "<noresponse></noresponse>");
  }
}

void handleLoginData(AsyncWebServerRequest *request)
{
  // Log.verbose("[Web]: handleData Request %s %s received from %p" CR, request->methodToString(), request->url().c_str(), request->client()->remoteIP());

  if (request->_tempObject != nullptr)
  {
    // Log.verbose("[Web]: handleData _tempObject %s" CR, request->_tempObject);
    String *bodyPtr = (String *)request->_tempObject;
    String body = *bodyPtr;
    // Log.verbose("[Web]: handleData 1" CR);
    delete bodyPtr;
    // Log.verbose("[Web]: handleData 2" CR);
    request->_tempObject = nullptr;
    // Log.verbose("[Web]: handleData body %s" CR, body.c_str());

    // data.device.device_id
    // data.token

    AsyncResponseStream *response = request->beginResponseStream("application/json");
    DynamicJsonDocument doc(128);

    doc["username"] = WiFi.getHostname();
    doc["token"] = WiFi.macAddress();
    doc["device"]["device_id"] = WiFi.macAddress();

    serializeJsonPretty(doc, *response);
    request->send(response);

    Log.verbose("[Web]: handleData %p %s %s %s" CR, request->client()->remoteIP(), request->methodToString(), request->url().c_str(), response);
  }
  else
  {
    Log.verbose("[Web]: handleData no body" CR);
    request->send(200, "text/xml", "<noresponse></noresponse>");
  }
}

void handleOptionsData(AsyncWebServerRequest *request)
{
  Log.verbose("[Web]: handleOptionsData %p %s %s" CR, request->client()->remoteIP(), request->methodToString(), request->url().c_str());
  request->send(200, "text/plain", "Data received");
}

void handleOptionsLoginData(AsyncWebServerRequest *request)
{
  Log.verbose("[Web]: handleOptionsLoginData %p %s %s" CR, request->client()->remoteIP(), request->methodToString(), request->url().c_str());
  request->send(200, "text/plain", "Data received");
}

void handleSlash(AsyncWebServerRequest *request)
{
  Log.verbose("[Web]: handleSlash %p %s %s" CR, request->client()->remoteIP(), request->methodToString(), request->url().c_str());
  AsyncWebServerResponse *response = request->beginResponse(302); // Sends 302 Weiterleitung
  response->addHeader("Location", "index.html");
  request->send(response);
}

/** Cache-Control for static LittleFS files: hashed SPA chunks vs entry HTML. */
static void addCacheControlForLittleFsPath(AsyncWebServerResponse *response, const String &urlPath)
{
  if (!response)
  {
    return;
  }
  const char *cacheControl;
  if (urlPath.indexOf("/assets/") >= 0)
  {
    cacheControl = "public, max-age=31536000, immutable";
  }
  else if (urlPath == "/index.html" || urlPath.endsWith("/index.html"))
  {
    cacheControl = "no-cache";
  }
  else
  {
    cacheControl = "public, max-age=3600";
  }
  response->addHeader("Cache-Control", cacheControl);
}

static void sendLittleFsFileWithCache(AsyncWebServerRequest *request, const String &path)
{
  AsyncWebServerResponse *response = request->beginResponse(LittleFS, path, String(), false);
  addCacheControlForLittleFsPath(response, path);
  request->send(response);
}

void handleNotFound(AsyncWebServerRequest *request)
{
  if (LittleFS.exists(request->url()))
  {
    Log.verbose("[Web]: LFS %p %s %s" CR, request->client()->remoteIP(), request->methodToString(), request->url().c_str());
    sendLittleFsFileWithCache(request, request->url());
    return;
  }

  Log.verbose(F("[Web]: handleNotFound() %s %s" CR), request->methodToString(), request->url().c_str());
  int headers = request->headers();
  int i;
  for (i = 0; i < headers; i++)
  {
    const AsyncWebHeader *h = request->getHeader(i);  // Add 'const' here
    Log.verbose("HEADER[%s]: %s\n", h->name().c_str(), h->value().c_str());
  }

  int args = request->args();
  for (int i = 0; i < args; i++)
  {
    Log.verbose(F("ARG[%s]: %s" CR), request->argName(i).c_str(), request->arg(i).c_str());
  }

  request->send(404, "text/plain", "Not found");
}

void listDir(fs::FS &fs, const char *dirname, uint8_t levels)
{
  File root = fs.open(dirname);
  if (!root)
  {
    Serial.println("- failed to open directory");
    return;
  }
  if (!root.isDirectory())
  {
    Serial.println(" - not a directory");
    return;
  }

  File file = root.openNextFile();
  while (file)
  {
    if (file.isDirectory())
    {
      if (levels)
      {
        listDir(fs, file.path(), levels - 1);
      }
    }
    else
    {
      Serial.print("  FILE: ");
      Serial.print(dirname);
      if (strcmp(dirname, "/") != 0)
      {
        Serial.print("/");
      }
      Serial.print(file.name());
      Serial.print("\tSIZE: ");
      Serial.println(file.size());
    }
    file = root.openNextFile();
  }
}

String listDirToString(fs::FS &fs, const char *dirname, uint8_t levels)
{
  String response = "";
  File root = fs.open(dirname);
  if (!root)
  {
    // Serial.println("- failed to open directory");
    return String("");
  }
  if (!root.isDirectory())
  {
    // Serial.println(" - not a directory");
    return String("");
  }

  File file = root.openNextFile();
  while (file)
  {
    if (file.isDirectory())
    {
      if (levels)
      {
        response += listDirToString(fs, file.path(), levels - 1);
      }
    }
    else
    {
      response += "  FILE: " + String(dirname);
      if (strcmp(dirname, "/") != 0)
      {
        response += "/";
      }
      response += String(file.name()) + "\tSIZE: " + String(file.size()) + "<BR>";
    }
    file = root.openNextFile();
  }
  return response;
}
