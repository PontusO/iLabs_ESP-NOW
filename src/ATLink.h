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
   */
  int command(const char *cmd, LineCb onLine = nullptr, void *arg = nullptr, uint32_t timeout_ms = 0);

  /* Non-blocking: read and dispatch any pending asynchronous URC lines. */
  void poll();

  /* Register the asynchronous-URC handler (installed by the ESP_NOW layer). */
  void onURC(LineCb cb, void *arg) {
    _urc_cb = cb;
    _urc_arg = arg;
  }

  static bool isAsyncURC(const char *line);

private:
  const char *readLine(uint32_t timeout_ms);
  void dispatchURC(const char *line) {
    if (_urc_cb) {
      _urc_cb(line, _urc_arg);
    }
  }

  Stream *_s;
  uint8_t _chan;
  bool _started;

  char _acc[ILABS_ESPNOW_LINE_MAX];
  size_t _acc_len;
  bool _overflow;

  LineCb _urc_cb;
  void *_urc_arg;
};
