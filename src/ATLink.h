/*
 * ATLink.h - AT+EN transport client for the ESP32 ESP-NOW co-processor.
 *
 * Thin line-oriented client for the iLabs AT+EN protocol spoken by an
 * ESP32-C6/C3 slave over a Serial link. Handles command/response framing
 * (OK / ERROR / +ENERR:<n>) and demultiplexes asynchronous URCs
 * (+ENRECV, +ENSENDOK/+ENSENDFAIL, ...) from synchronous query results.
 *
 * Single-threaded and cooperative: command() blocks (with a timeout) until
 * the terminal OK/ERROR, dispatching any URCs that interleave; poll() drains
 * pending URCs without blocking. The ESP_NOW class layer sits on top.
 */

#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Max accepted AT line length (bytes). Bounds the largest +ENRECV the host
 * will reassemble: "+ENRECV:" + fields + 2*payload hex + dst MAC. 640 covers
 * a full 250-byte single frame; raise it (build flag) for fragmented RX. */
#ifndef ILABS_ESPNOW_LINE_MAX
#define ILABS_ESPNOW_LINE_MAX 640
#endif

/* Default per-command wait for the terminal OK/ERROR (ms). */
#ifndef ILABS_ESPNOW_CMD_TIMEOUT_MS
#define ILABS_ESPNOW_CMD_TIMEOUT_MS 1500
#endif

/* Max wait for the co-processor's +ENREADY after a hardware reset (ms). */
#ifndef ILABS_ESPNOW_READY_TIMEOUT_MS
#define ILABS_ESPNOW_READY_TIMEOUT_MS 3000
#endif

/* Max AT commands that may be queued from within receive callbacks before
 * the transport unwinds and runs them (reentrancy safety). */
#ifndef ILABS_ESPNOW_DEFER_MAX
#define ILABS_ESPNOW_DEFER_MAX 8
#endif

/*
 * Asynchronous URC prefixes: the unsolicited result codes the co-processor
 * emits outside the command/response exchange. Single source of truth - both
 * the transport classifier (ATLink::isAsyncURC, which decides what to route to
 * the URC handler) and the ESP_NOW dispatcher (urc_handler) match against these
 * exact tokens, so a new URC is added in one place and no hand-counted lengths
 * can drift. The tokens are colon-less bases; the co-processor appends
 * ":<fields>" to all but +ENREADY.
 */
#define ILABS_URC_RECV     "+ENRECV"
#define ILABS_URC_SENDOK   "+ENSENDOK"
#define ILABS_URC_SENDFAIL "+ENSENDFAIL"
#define ILABS_URC_FRAGRECV "+ENFRAGRECV"
#define ILABS_URC_READY    "+ENREADY"

/* Does `s` begin with `prefix`? For a string literal, strlen() folds to a
 * compile-time constant, so this is as cheap as a hand-counted strncmp but
 * cannot miscount. Shared by both library layers (see ATLink.cpp / ESP32_NOW.cpp). */
static inline bool ilabs_starts_with(const char *s, const char *prefix) {
  return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* If `line` begins with "tag:", return a pointer just past the colon (the
 * payload); otherwise nullptr. Replaces hand-counted strncmp offsets for every
 * tagged result/URC/error line ("+ENMAC:", "+ENRECV:", "+ENERR:", ...) - the
 * offset can't drift from the tag string. For colon-less markers (+ENREADY)
 * use ilabs_starts_with instead. */
static inline const char *ilabs_after_tag(const char *line, const char *tag) {
  size_t n = strlen(tag);
  if (strncmp(line, tag, n) == 0 && line[n] == ':') {
    return line + n + 1;
  }
  return nullptr;
}

class ATLink {
public:
  typedef void (*LineCb)(const char *line, void *arg);

  ATLink();

  void begin(Stream &serial, uint8_t channel);
  bool started() const {
    return _started;
  }
  uint8_t channel() const {
    return _chan;
  }
  Stream *stream() const {
    return _s;
  }

  /*
   * Send one AT command line (CRLF appended) and wait for its terminal
   * response. Intermediate query-result lines (e.g. +ENMAC:, +ENVER:,
   * +ENLISTPEER:) are delivered to onLine; asynchronous URCs that arrive
   * in between are routed to the URC handler instead.
   *
   * Returns  0  on OK,
   *         >0  the +ENERR:<n> code on a coded error,
   *         -1  on plain ERROR,
   *         -2  on timeout / link not started.
   *
   * Reentrancy: if called from within a URC callback (i.e. while the transport
   * is dispatching a received line), the command is queued and executed after
   * the dispatch unwinds, and this returns 0 (optimistic). Callbacks should
   * therefore only issue fire-and-forget commands (send/add/remove/setKey),
   * not queries whose result they need synchronously.
   */
  int command(const char *cmd, LineCb onLine = nullptr, void *arg = nullptr, uint32_t timeout_ms = 0);

  /* Non-blocking: read and dispatch any pending asynchronous URC lines. */
  void poll();

  /* Drain any buffered input and reset the line assembler. */
  void flushInput();

  /* Block until the firmware's +ENREADY boot marker, discarding everything
   * else (e.g. ROM boot chatter). Returns false on timeout. */
  bool waitReady(uint32_t timeout_ms);

  /* Register the asynchronous-URC handler (installed by the ESP_NOW layer). */
  void onURC(LineCb cb, void *arg) {
    _urc_cb = cb;
    _urc_arg = arg;
  }

  static bool isAsyncURC(const char *line);

private:
  const char *readLine(uint32_t timeout_ms);

  // The actual command execution (write + wait for terminal response).
  int runCommand(const char *cmd, LineCb onLine, void *arg, uint32_t timeout_ms);

  // Run a URC callback with the dispatch-depth guard raised, so any command()
  // the callback issues is deferred rather than run reentrantly.
  void dispatchURC(const char *line) {
    if (_urc_cb) {
      _dispatch_depth++;
      _urc_cb(line, _urc_arg);
      _dispatch_depth--;
    }
  }

  void deferCommand(const char *cmd);
  void drainDeferred();

  Stream *_s;
  uint8_t _chan;
  bool _started;

  char _acc[ILABS_ESPNOW_LINE_MAX];
  size_t _acc_len;
  bool _overflow;

  LineCb _urc_cb;
  void *_urc_arg;

  int _dispatch_depth;  // >0 while running a URC callback
  bool _draining;       // guards drainDeferred() against re-entry

  // FIFO of commands deferred from callbacks (heap copies, run FIFO).
  char *_deferred[ILABS_ESPNOW_DEFER_MAX];
  int _defer_head;
  int _defer_tail;
  int _defer_count;
};
