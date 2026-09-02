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
#include "spaPortalAssets.h"

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
  AsyncWebServerResponse *response = request->beginResponse(
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


/** Serve one flash-resident portal asset with immutable caching (URLs carry ?v=VERSION). */
static void spaWebSendPortalAsset(AsyncWebServerRequest *request, const char *contentType,
                                  const char *bodyData, size_t bodyLen)
{
  AsyncWebServerResponse *response =
      request->beginResponse(200, contentType, reinterpret_cast<const uint8_t *>(bodyData), bodyLen);
  if (response == nullptr)
  {
    request->send(503, "text/plain", "asset unavailable");
    return;
  }
  response->addHeader("Cache-Control", "public, max-age=31536000, immutable");
  request->send(response);
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

    // Flash-resident shared portal assets (see spaPortalAssets.h).
    server.on("/assets/portal.css", HTTP_GET, [](AsyncWebServerRequest *request) {
      spaWebSendPortalAsset(request, "text/css", kPortalCss, sizeof(kPortalCss) - 1);
    });
    server.on("/assets/portal-head.js", HTTP_GET, [](AsyncWebServerRequest *request) {
      spaWebSendPortalAsset(request, "application/javascript", kPortalHeadJs, sizeof(kPortalHeadJs) - 1);
    });
    server.on("/assets/portal-status.js", HTTP_GET, [](AsyncWebServerRequest *request) {
      spaWebSendPortalAsset(request, "application/javascript", kPortalStatusJs, sizeof(kPortalStatusJs) - 1);
    });
    server.on("/assets/portal-config.js", HTTP_GET, [](AsyncWebServerRequest *request) {
      spaWebSendPortalAsset(request, "application/javascript", kPortalConfigJs, sizeof(kPortalConfigJs) - 1);
    });
    server.on("/assets/portal-state.js", HTTP_GET, [](AsyncWebServerRequest *request) {
      spaWebSendPortalAsset(request, "application/javascript", kPortalStateJs, sizeof(kPortalStateJs) - 1);
    });
    server.on("/assets/portal-logs.js", HTTP_GET, [](AsyncWebServerRequest *request) {
      spaWebSendPortalAsset(request, "application/javascript", kPortalLogsJs, sizeof(kPortalLogsJs) - 1);
    });

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


#define portalHeadIcon "<link rel='icon' href='/assets/style/hottubbing.webp' type='image/x-icon' />"



#define portalThemeToggleBtn "<button type=\"button\" class=\"portal-theme-toggle\" data-portal-theme-toggle onclick=\"portalCycleTheme()\" aria-label=\"Theme: Auto. Click to change.\"><span class=\"portal-theme-toggle__icon\" aria-hidden=\"true\"><svg xmlns=\"http://www.w3.org/2000/svg\" width=\"18\" height=\"18\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" aria-hidden=\"true\"><circle cx=\"12\" cy=\"12\" r=\"10\"/><path d=\"M12 2a10 10 0 0 0 0 20V2z\" fill=\"currentColor\" stroke=\"none\"/></svg></span><span class=\"portal-theme-toggle__label\">Auto</span></button>"



/** True when portal global CSS marker is present (String: buffer scan; chunks: append stayed healthy). */
static bool portalHtmlSawGlobalCss(const String &html)
{
  // Global CSS is a flash-served asset now; presence means the <link> made it into the head.
  return html.indexOf(F("/assets/portal.css")) >= 0;
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
  // Shared portal CSS/JS live in flash as static cacheable assets (spaPortalAssets.h). The
  // ~58KB of styles/scripts previously inlined here cost 14 RAM slabs on every page build.
  html += F("<link rel='stylesheet' href='/assets/portal.css?v=" VERSION "'>");
  const bool sawPortalCss = portalHtmlSawGlobalCss(html);
  html += F("<script src='/assets/portal-head.js?v=" VERSION "'></script>");
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


  // Build `<head>` with sequential appends (see `appendPortalHead`).
  html = F("<html>");
  const bool wroteOpeningTag = html.healthy() && html.length() >= 6;
  const bool sawPortalCss = appendPortalHead(html, "Spa Status");
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

  html += F("<script src='/assets/portal-status.js?v=" VERSION "'></script>");
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



  PortalHtmlChunks html;
  html = F("<html>");
  const bool sawPortalCss = appendPortalHead(html, "Spa Config");
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

  html += F("<script src='/assets/portal-config.js?v=" VERSION "'></script>");

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
  PortalHtmlChunks html;
  html = F("<html>");
  const bool sawPortalCss = appendPortalHead(html, "ESP State");
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
  html += "</section></div>";



  html += F("<script src='/assets/portal-state.js?v=" VERSION "'></script></main></div></body></html>");

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
  const bool sawPortalCss = appendPortalHead(html, "Spa Logs", ",viewport-fit=cover");
  html += F("<body class=\"logs-portal\"><a class='skip-link' href='#mainContent'>Skip to main content</a><div class='page logs-page'>");
  html += webMenuLogs;
  html += F("<main id='mainContent'><section class='panel logs-panel'><div class='logs-stack'><h1>Device logs</h1>");
  html += "<p style='color:var(--muted);font-size:14px;margin-top:0'>Recent lines are buffered on the gateway; include/exclude filters run in the browser. Logs are teed to USB <code>Serial</code> (monitor baud) and this ring. For a live tail without USB, use this page or <code>GET /api/logs</code> (optional WebSocket tail). If the firmware was built with <code>TELNET_LOG</code>, <code>TelnetStream</code> also listens on TCP port 23; the global logger is <em>not</em> switched to Telnet (see Wi‑Fi boot messages).</p>";

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
  html += "<div id='logView' class='log-view' aria-live='polite'></div></section></main></div>";
  html += F("<script src='/assets/portal-logs.js?v=" VERSION "'></script></body></html>");

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
