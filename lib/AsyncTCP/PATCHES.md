# Vendored AsyncTCP — local modifications

- **Upstream baseline**: ESP32Async/AsyncTCP **v3.3.8** (https://github.com/ESP32Async/AsyncTCP)
- **License**: LGPL-3.0 (see LICENSE — unmodified). This file is the prominent notice of
  modification required by the license.
- **Vendored**: 2026-09-01, into `lib/AsyncTCP/` of esp32_balboa_spa. `dependencies` were
  stripped from `library.json` (they pulled ESP8266/RP2040 platform code into PlatformIO
  builds); `version` marked `3.3.8-spa-patched`.

## Why it is vendored at all

On the arduino-esp32 2.x core (IDF 4.4, `CONFIG_LWIP_TCPIP_CORE_LOCKING` **not** set),
AsyncTCP 3.4.x/3.5.x freeze the whole network stack within a handful of large HTTP transfers
(their `tcp_core_guard` compiles to a no-op without core locking). 3.3.8 is the last line that
behaves, and it still had the deadlock/loss issues patched below. Do not upgrade the library or
the core without re-testing `docs` bench notes.

## Modifications (all in `src/AsyncTCP.cpp`, marked `Local patch:` in code)

### 2026-09-01
- Bounded queue posting: `_send_async_event`/`_prepend_async_event` default wait changed from
  `portMAX_DELAY` to `CONFIG_ASYNC_TCP_EVENT_WAIT_MS` (100 ms). An eternal wait deadlocked the
  tcpip task against the async task (AB-BA via `tcpip_api_call`), taking the device off the
  network permanently. Bench-reproduced within ~15 chunked 73 KB transfers.
- `_tcp_clear_events`: no longer blocks forever prepending the CLEAR marker.

### 2026-09-02 (external review follow-up)
- Event classes split by loss semantics: RECV/POLL are retryable (lwIP redelivers refused
  data; polls repeat) and must leave `CONFIG_ASYNC_TCP_QUEUE_RESERVE` (32) queue slots free;
  SENT/FIN/ERROR/CONNECTED/DNS/ACCEPT/CLEAR are never redelivered by lwIP and now use the
  reserved slots plus a longer bounded wait (`CONFIG_ASYNC_TCP_CONTROL_EVENT_WAIT_MS`, 1 s),
  with loud logs if one is ever lost. Previously a data flood could crowd out a SENT/FIN
  event, permanently stalling ack accounting or leaking the connection object.
- `_free_event()`: discarding a queued `LWIP_TCP_RECV` event now frees its pbuf chain
  (previously leaked by `_remove_events_with_arg`). RECV enqueue *refusal* still frees only
  the wrapper — lwIP retains pbuf ownership for redelivery.
- `_tcp_clear_events` is task-aware: on the async task it purges synchronously (prepending
  could self-deadlock); from any other task it hands the purge to the consumer via the CLEAR
  marker so queue rotation cannot race the consumer's dequeues.
