/*
 * ATLink.cpp - AT+EN transport client implementation.
 */

#include "ATLink.h"
#include <string.h>
#include <stdlib.h>

ATLink::ATLink()
  : _s(nullptr), _chan(1), _started(false), _acc_len(0), _overflow(false), _urc_cb(nullptr), _urc_arg(nullptr),
    _dispatch_depth(0), _draining(false), _defer_head(0), _defer_tail(0), _defer_count(0) {
  for (int i = 0; i < ILABS_ESPNOW_DEFER_MAX; i++) {
    _deferred[i] = nullptr;
  }
}

void ATLink::begin(Stream &serial, uint8_t channel) {
  _s = &serial;
  _chan = channel;
  _acc_len = 0;
  _overflow = false;
  _started = true;
}

bool ATLink::isAsyncURC(const char *line) {
  return ilabs_starts_with(line, ILABS_URC_RECV) || ilabs_starts_with(line, ILABS_URC_SENDOK)
         || ilabs_starts_with(line, ILABS_URC_SENDFAIL) || ilabs_starts_with(line, ILABS_URC_FRAGRECV)
         || ilabs_starts_with(line, ILABS_URC_READY);
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
  // Issued from within a URC callback: defer to avoid reentrant transport use.
  // (onLine results can't be delivered to a deferred caller - see the header.)
  if (_dispatch_depth > 0) {
    deferCommand(cmd);
    return 0;
  }

  int r = runCommand(cmd, onLine, arg, timeout_ms);
  drainDeferred();
  return r;
}

int ATLink::runCommand(const char *cmd, LineCb onLine, void *arg, uint32_t timeout_ms) {
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
    const char *enerr = ilabs_after_tag(line, "+ENERR");
    if (enerr) {
      pending_enerr = atoi(enerr);
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
  if (_dispatch_depth > 0) {
    return;  // don't re-enter poll() from within a callback
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
  drainDeferred();
}

// Queue a copy of a command issued from within a callback (FIFO).
void ATLink::deferCommand(const char *cmd) {
  if (_defer_count >= ILABS_ESPNOW_DEFER_MAX) {
    return;  // queue full: drop (callbacks shouldn't burst this many commands)
  }
  char *copy = strdup(cmd);
  if (!copy) {
    return;
  }
  _deferred[_defer_tail] = copy;
  _defer_tail = (_defer_tail + 1) % ILABS_ESPNOW_DEFER_MAX;
  _defer_count++;
}

// Run any queued callback-initiated commands, now that dispatch has unwound.
// Commands run here may themselves dispatch URCs whose callbacks enqueue more,
// which this same loop then drains (FIFO); _draining guards against re-entry.
void ATLink::drainDeferred() {
  if (_draining || _dispatch_depth > 0) {
    return;
  }
  _draining = true;
  while (_defer_count > 0) {
    char *cmd = _deferred[_defer_head];
    _deferred[_defer_head] = nullptr;
    _defer_head = (_defer_head + 1) % ILABS_ESPNOW_DEFER_MAX;
    _defer_count--;
    runCommand(cmd, nullptr, nullptr, 0);
    free(cmd);
  }
  _draining = false;
}

void ATLink::flushInput() {
  if (_s) {
    while (_s->available() > 0) {
      _s->read();
    }
  }
  _acc_len = 0;
  _overflow = false;
}

bool ATLink::waitReady(uint32_t timeout_ms) {
  uint32_t start = millis();
  for (;;) {
    uint32_t elapsed = millis() - start;
    if (elapsed >= timeout_ms) {
      return false;
    }
    const char *line = readLine(timeout_ms - elapsed);
    if (line && ilabs_starts_with(line, ILABS_URC_READY)) {
      return true;
    }
    // discard boot ROM chatter / stray lines and keep waiting
  }
}
