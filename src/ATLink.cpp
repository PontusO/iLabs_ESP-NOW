/*
 * ATLink.cpp - AT+EN transport client implementation.
 */

#include "ATLink.h"
#include <string.h>
#include <stdlib.h>

ATLink::ATLink()
  : _s(nullptr), _chan(1), _started(false), _acc_len(0), _overflow(false), _urc_cb(nullptr), _urc_arg(nullptr) {}

void ATLink::begin(Stream &serial, uint8_t channel) {
  _s = &serial;
  _chan = channel;
  _acc_len = 0;
  _overflow = false;
  _started = true;
}

bool ATLink::isAsyncURC(const char *line) {
  return strncmp(line, "+ENRECV", 7) == 0 || strncmp(line, "+ENSENDOK", 9) == 0 || strncmp(line, "+ENSENDFAIL", 11) == 0
         || strncmp(line, "+ENFRAGRECV", 11) == 0 || strncmp(line, "+ENREADY", 8) == 0;
}

/*
 * Assemble one CR/LF-terminated line from the stream. Partial input is kept
 * in _acc across calls, so this is safe to call with a zero timeout from
 * poll(). Over-length lines are discarded up to the next newline.
 */
const char *ATLink::readLine(uint32_t timeout_ms) {
  if (!_s) {
    return nullptr;
  }
  uint32_t start = millis();
  for (;;) {
    while (_s->available() > 0) {
      char c = (char)_s->read();
      if (c == '\n' || c == '\r') {
        if (_overflow) {
          _overflow = false;
          _acc_len = 0;
          continue;
        }
        if (_acc_len > 0) {
          _acc[_acc_len] = '\0';
          _acc_len = 0;
          return _acc;
        }
        continue;  // ignore empty line (bare CR/LF pair)
      }
      if (_overflow) {
        continue;  // dropping an over-length line until its newline
      }
      if (_acc_len < ILABS_ESPNOW_LINE_MAX - 1) {
        _acc[_acc_len++] = c;
      } else {
        _overflow = true;
        _acc_len = 0;
      }
    }
    if ((millis() - start) >= timeout_ms) {
      return nullptr;
    }
    yield();
  }
}

int ATLink::command(const char *cmd, LineCb onLine, void *arg, uint32_t timeout_ms) {
  if (!_started || !_s) {
    return -2;
  }
  if (timeout_ms == 0) {
    timeout_ms = ILABS_ESPNOW_CMD_TIMEOUT_MS;
  }

  _s->print(cmd);
  _s->print("\r\n");

  int pending_enerr = 0;
  uint32_t start = millis();
  for (;;) {
    uint32_t elapsed = millis() - start;
    if (elapsed >= timeout_ms) {
      return -2;
    }
    const char *line = readLine(timeout_ms - elapsed);
    if (!line) {
      return -2;
    }
    if (strcmp(line, "OK") == 0) {
      return 0;
    }
    if (strcmp(line, "ERROR") == 0) {
      return pending_enerr > 0 ? pending_enerr : -1;
    }
    if (strncmp(line, "+ENERR:", 7) == 0) {
      pending_enerr = atoi(line + 7);
      continue;
    }
    if (isAsyncURC(line)) {
      dispatchURC(line);
      continue;
    }
    if (onLine) {
      onLine(line, arg);
    }
    // otherwise: unexpected intermediate line, ignore
  }
}

void ATLink::poll() {
  if (!_started) {
    return;
  }
  for (;;) {
    const char *line = readLine(0);
    if (!line) {
      break;
    }
    if (isAsyncURC(line)) {
      dispatchURC(line);
    }
    // ignore stray OK/ERROR/result lines outside a command
  }
}
