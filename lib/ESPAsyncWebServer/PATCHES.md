# Vendored ESPAsyncWebServer — local modifications

- **Upstream baseline**: me-no-dev/ESPAsyncWebServer commit **ad3741d** (library metadata
  v3.6.0 lineage, https://github.com/me-no-dev/ESPAsyncWebServer)
- **License**: LGPL-3.0 (see LICENSE — unmodified). This file is the prominent notice of
  modification required by the license.
- **Vendored**: 2026-09-01, into `lib/ESPAsyncWebServer/` of esp32_balboa_spa. `dependencies`
  were stripped from `library.json` (they pulled ESP8266/RP2040 platform code into PlatformIO
  builds); `version` marked `3.6.0-spa-patched`.
- Pairs with the vendored AsyncTCP 3.3.8 in `lib/AsyncTCP/` (see its PATCHES.md for why the
  ESP32Async 3.9+/3.4.10+ combination cannot be used on the arduino-esp32 2.x core).

## Modifications (marked `Local patch:` in code)

### 2026-09-01
- `src/WebResponseImpl.h` / `src/WebResponses.cpp` — carry buffer for
  `AsyncAbstractResponse::_ack`: upstream removed a full fill-sized range from the response
  even when `write()` accepted none/part of it (tcp_write `ERR_MEM` under pbuf/heap pressure),
  corrupting large chunked portal pages. Unsent wire bytes are now retained
  (`_carryBuf/_carryLen/_carryOff/_carryTerminal`) and flushed before any new fill, keeping the
  stream byte-identical. (Upstream fixed the same class of bug in ESP32Async v3.9.0, which
  cannot be used here — see AsyncTCP PATCHES.md.)

### 2026-09-02 (external review follow-up)
- `src/WebResponses.cpp` — all response send paths (`AsyncBasicResponse::_respond/_ack`,
  `AsyncAbstractResponse::_ack` header/carry/content) now use `_acceptedWrite()` =
  `AsyncClient::add()` + best-effort `send()` instead of `write()`. `write()` returns 0 when
  only `tcp_output` fails *after* `tcp_write` queued the bytes; retrying those duplicated
  bytes on the wire. `add()`'s accepted count is authoritative, and exactly the unaccepted
  remainder is retained (including partial HTTP heads, which upstream dropped blindly).
- `src/WebRequest.cpp` — the `beginResponse*` family allocates with `new (std::nothrow)`
  (under `-fno-exceptions` a failed throwing `new` aborts the firmware; these run on
  OOM-recovery paths where returning nullptr is the point). Null-response guards added where
  this library itself dereferences the result (request completion, `redirect`).
