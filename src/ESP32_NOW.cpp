/*
 * ESP32_NOW.cpp - Arduino ESP-NOW class API over the iLabs AT+EN link.
 *
 * Maps the arduino-esp32 ESP_NOW class surface onto AT commands sent to an
 * ESP32-C6/C3 co-processor, and demultiplexes the +ENRECV / send-status URCs
 * back into the ESP_NOW_Peer onReceive()/onSent() callbacks.
 */

#include <Arduino.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "ESP32_NOW.h"
#include "ATLink.h"

/* ---- module state -------------------------------------------------- */

static ATLink g_link;
static ESP_NOW_Peer *_peers[ESP_NOW_MAX_TOTAL_PEER_NUM];
static bool _has_begun = false;

// Negotiated at begin(). File-static like the rest of the module state: the
// ESP_NOW_Class is a singleton (matching arduino-esp32), so it holds no
// per-instance data of its own.
static size_t _max_data_len = 0;
static uint32_t _version = 0;

static void (*new_cb)(const esp_now_recv_info_t *info, const uint8_t *data, int len, void *arg) = nullptr;
static void *new_arg = nullptr;

// Set when the co-processor's +ENREADY is seen during operation (unexpected
// reboot). Not set for the boot +ENREADY consumed by setLink()'s waitReady().
static volatile bool _link_reset_flag = false;
static void (*reset_cb)(void *arg) = nullptr;
static void *reset_arg = nullptr;

/* ---- small helpers ------------------------------------------------- */

static const char HEXD[] = "0123456789ABCDEF";

// Write 2*n uppercase hex chars + a NUL at out.
static void bytes_to_hex(const uint8_t *b, size_t n, char *out) {
  for (size_t i = 0; i < n; i++) {
    out[2 * i] = HEXD[b[i] >> 4];
    out[2 * i + 1] = HEXD[b[i] & 0x0F];
  }
  out[2 * n] = '\0';
}

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

static bool hex_to_bytes(const char *h, size_t nbytes, uint8_t *out) {
  for (size_t i = 0; i < nbytes; i++) {
    int hi = hexval(h[2 * i]);
    int lo = hexval(h[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

static void mac_to_hex(const uint8_t m[6], char out[13]) {
  bytes_to_hex(m, 6, out);
}

// Parse the first 12 chars of s as a MAC (no separators).
static bool parse_mac(const char *s, uint8_t m[6]) {
  for (int i = 0; i < 12; i++) {
    if (hexval(s[i]) < 0) return false;
  }
  return hex_to_bytes(s, 6, m);
}

static bool is_broadcast(const uint8_t m[6]) {
  for (int i = 0; i < 6; i++) {
    if (m[i] != 0xFF) return false;
  }
  return true;
}

// Largest payload carried in a single ESP-NOW frame over the AT bridge: the
// v1 max (250) minus the 2-byte iLabs frame header. Unicast sends above this
// are fragmented/reassembled by the firmware; broadcast (AT+ENBCAST) has no
// fragmentation and is capped here.
static const size_t ESPNOW_SINGLE_FRAME_MAX = ESP_NOW_MAX_DATA_LEN - 2;

// Largest AT command line we build on the send path: an "AT+ENFRAGSEND=<mac>,
// <len>," prefix (~31 chars) followed by 2*ESP_NOW_MAX_DATA_LEN payload hex.
// Comfortably covers every command_hex() caller, so the send path never mallocs.
#define ILABS_AT_CMD_MAX (64 + 2 * ESP_NOW_MAX_DATA_LEN)

// Append `len` bytes of `data` as uppercase hex to `prefix` (which already
// holds everything up to and including the trailing comma) and send the
// resulting AT command. Everything lands in a stack buffer. Returns the
// g_link.command() result (0 = OK, <0 / >0 = error).
static int command_hex(const char *prefix, const uint8_t *data, size_t len) {
  char cmd[ILABS_AT_CMD_MAX];
  size_t plen = strlen(prefix);
  if (plen + 2 * len + 1 > sizeof(cmd)) {
    return -1;  // caller bounds len <= ESP_NOW_MAX_DATA_LEN; guard regardless
  }
  memcpy(cmd, prefix, plen);
  bytes_to_hex(data, len, cmd + plen);
  return g_link.command(cmd);
}

static ESP_NOW_Peer *find_peer(const uint8_t *mac) {
  for (int i = 0; i < ESP_NOW_MAX_TOTAL_PEER_NUM; i++) {
    if (_peers[i] != nullptr && memcmp(_peers[i]->addr(), mac, 6) == 0) {
      return _peers[i];
    }
  }
  return nullptr;
}

// AT+ENADDPEER=<mac>,<chan>,<enc>[,<lmk>]. Also modifies an existing peer
// (the firmware upgrades a duplicate ADDPEER in place).
static int at_addpeer(const uint8_t mac[6], uint8_t chan, bool encrypt, const uint8_t key[16]) {
  char mstr[13];
  mac_to_hex(mac, mstr);
  char prefix[48];
  if (encrypt) {
    snprintf(prefix, sizeof(prefix), "AT+ENADDPEER=%s,%u,1,", mstr, (unsigned)chan);
    return command_hex(prefix, key, 16);
  }
  snprintf(prefix, sizeof(prefix), "AT+ENADDPEER=%s,%u,0", mstr, (unsigned)chan);
  return g_link.command(prefix);
}

/* ---- URC dispatch (called by ATLink from command()/poll()) --------- */

static void urc_handler(const char *line, void *arg) {
  (void)arg;

  if (strncmp(line, "+ENRECV:", 8) == 0) {
    // src,len,rssi,payload_hex[,dst]
    char buf[ILABS_ESPNOW_LINE_MAX];
    size_t blen = strlen(line + 8);
    if (blen >= sizeof(buf)) return;  // line is already length-bounded by ATLink
    memcpy(buf, line + 8, blen + 1);

    char *f[5];
    int nf = 0;
    char *p = buf;
    while (nf < 5) {
      f[nf++] = p;
      char *c = strchr(p, ',');
      if (!c) break;
      *c = '\0';
      p = c + 1;
    }
    if (nf < 4) return;

    uint8_t src[6];
    if (!parse_mac(f[0], src)) return;
    int len = atoi(f[1]);
    // f[2] = rssi (not surfaced by the ESP-NOW callback API)
    const char *hex = f[3];
    if (len < 0 || strlen(hex) != (size_t)len * 2) return;

    uint8_t dst[6];
    bool have_dst = false, bcast = false;
    if (nf >= 5 && parse_mac(f[4], dst)) {
      have_dst = true;
      bcast = is_broadcast(dst);
    }

    // Decode into a stack buffer (no per-frame malloc). `hex` points into buf,
    // so len is bounded by the line length: len <= (ILABS_ESPNOW_LINE_MAX-8)/2.
    uint8_t payload[ILABS_ESPNOW_LINE_MAX / 2];
    if ((size_t)len > sizeof(payload)) return;
    if (len > 0 && !hex_to_bytes(hex, (size_t)len, payload)) return;

    ESP_NOW_Peer *pr = find_peer(src);
    if (pr) {
      pr->onReceive(payload, (size_t)len, bcast);
    } else if (new_cb) {
      uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
      esp_now_recv_info_t info;
      info.src_addr = src;
      info.des_addr = have_dst ? dst : zero;
      info.rx_ctrl = nullptr;
      new_cb(&info, payload, len, new_arg);
    }
    return;
  }

  if (strncmp(line, "+ENSENDOK:", 10) == 0) {
    uint8_t m[6];
    if (parse_mac(line + 10, m)) {
      ESP_NOW_Peer *pr = find_peer(m);
      if (pr) pr->onSent(true);
    }
    return;
  }

  if (strncmp(line, "+ENSENDFAIL:", 12) == 0) {
    uint8_t m[6];
    if (parse_mac(line + 12, m)) {
      ESP_NOW_Peer *pr = find_peer(m);
      if (pr) pr->onSent(false);
    }
    return;
  }

  if (strncmp(line, "+ENREADY", 8) == 0) {
    // The co-processor rebooted mid-operation: it has lost its peers/keys.
    _link_reset_flag = true;
    if (reset_cb) {
      reset_cb(reset_arg);
    }
    return;
  }

  // +ENFRAGRECV (progress) is informational; ignored here.
}

/* ---- query-result capture callbacks -------------------------------- */

struct MacCap {
  char mac[13];
  bool got;
};
static void cap_mac(const char *line, void *arg) {
  if (strncmp(line, "+ENMAC:", 7) == 0) {
    MacCap *m = (MacCap *)arg;
    strncpy(m->mac, line + 7, 12);
    m->mac[12] = '\0';
    m->got = true;
  }
}

struct VerCap {
  unsigned espnow_ver;
  bool got;
};
static void cap_ver(const char *line, void *arg) {
  if (strncmp(line, "+ENVER:", 7) == 0) {
    VerCap *v = (VerCap *)arg;
    const char *comma = strchr(line + 7, ',');
    v->espnow_ver = comma ? (unsigned)atoi(comma + 1) : 0;
    v->got = true;
  }
}

struct DiscoverCap {
  ESP_NOW_Found *out;
  int max;
  int count;
};
static void cap_discover(const char *line, void *arg) {
  // +ENDISCOVER:<mac>,<rssi>
  if (strncmp(line, "+ENDISCOVER:", 12) != 0) {
    return;
  }
  DiscoverCap *d = (DiscoverCap *)arg;
  if (d->count >= d->max) {
    return;
  }
  const char *p = line + 12;
  uint8_t mac[6];
  if (!parse_mac(p, mac)) {
    return;
  }
  const char *comma = strchr(p, ',');
  memcpy(d->out[d->count].mac, mac, 6);
  d->out[d->count].rssi = comma ? atoi(comma + 1) : 0;
  d->count++;
}

struct PeerCount {
  int total;
  int enc;
};
static void cap_peercount(const char *line, void *arg) {
  if (strncmp(line, "+ENLISTPEER:", 12) == 0) {
    PeerCount *pc = (PeerCount *)arg;
    pc->total++;
    // +ENLISTPEER:<mac>,<ch>,<enc>
    const char *c1 = strchr(line + 12, ',');
    if (!c1) return;
    const char *c2 = strchr(c1 + 1, ',');
    if (!c2) return;
    if (atoi(c2 + 1) == 1) pc->enc++;
  }
}

// Query the co-processor's peer table once (AT+ENLISTPEER?), returning both
// the total and encrypted counts. Callers that need only one still make a
// single round-trip. Returns false on link/command error.
static bool query_peer_counts(PeerCount &pc) {
  pc.total = 0;
  pc.enc = 0;
  return g_link.command("AT+ENLISTPEER?", cap_peercount, &pc) == 0;
}

/* ---- ESP_NOW_Class ------------------------------------------------- */

ESP_NOW_Class::ESP_NOW_Class() {
  _max_data_len = 0;
  _version = 0;
}

ESP_NOW_Class::~ESP_NOW_Class() {}

void ESP_NOW_Class::setLink(uint8_t channel) {
#ifndef ESP_SERIAL_PORT
#error "iLabs ESP-NOW requires a board variant that defines ESP_SERIAL_PORT (the UART wired to the ESP32 co-processor), e.g. an iLabs Challenger WiFi/WiFi6 board."
#else
  // Always the variant's ESP32 UART - the sketch never names a serial port.
  ESP_SERIAL_PORT.begin(ILABS_ESPNOW_LINK_BAUD);
  g_link.begin(ESP_SERIAL_PORT, channel);
  g_link.onURC(urc_handler, nullptr);

#if defined(PIN_ESP_MODE) && defined(PIN_ESP_RST)
  // Fully automatic ESP32 hardware reset into run mode, using the pins the
  // board variant defines. Gives a deterministic clean boot on every host
  // start (any peers/keys/baud from a prior run are cleared), and syncs the
  // host on the firmware's +ENREADY, discarding ROM boot chatter.
  pinMode(PIN_ESP_MODE, OUTPUT);
  digitalWrite(PIN_ESP_MODE, HIGH);  // run mode (not serial-download)
  pinMode(PIN_ESP_RST, OUTPUT);
  digitalWrite(PIN_ESP_RST, LOW);    // assert reset
  delay(5);
  g_link.flushInput();               // drop any pre-reset noise
  digitalWrite(PIN_ESP_RST, HIGH);   // release -> ESP32 boots
  g_link.waitReady(ILABS_ESPNOW_READY_TIMEOUT_MS);
  _link_reset_flag = false;          // the boot +ENREADY is expected, not a fault
#endif
#endif  // ESP_SERIAL_PORT
}

void ESP_NOW_Class::poll() {
  g_link.poll();
}

void ESP_NOW_Class::onReset(void (*cb)(void *arg), void *arg) {
  reset_cb = cb;
  reset_arg = arg;
}

bool ESP_NOW_Class::wasReset() {
  bool r = _link_reset_flag;
  _link_reset_flag = false;
  return r;
}

String ESP_NOW_Class::macAddress() {
  MacCap m = {{0}, false};
  g_link.command("AT+ENMAC?", cap_mac, &m);
  return m.got ? String(m.mac) : String();
}

int ESP_NOW_Class::discover(ESP_NOW_Found *out, int max, uint32_t timeout_ms) {
  if (!_has_begun) {
    log_e("ESP-NOW not initialized. Call begin() first.");
    return -1;
  }
  if (!out || max <= 0) {
    return -1;
  }

  char cmd[32];
  uint32_t window;
  if (timeout_ms == 0) {
    strcpy(cmd, "AT+ENDISCOVER");  // firmware default collection window (~1s)
    window = 1000;
  } else {
    snprintf(cmd, sizeof(cmd), "AT+ENDISCOVER=%lu", (unsigned long)timeout_ms);
    window = timeout_ms;
  }

  DiscoverCap cap = {out, max, 0};
  // The command blocks for the whole collection window; give it margin.
  if (g_link.command(cmd, cap_discover, &cap, window + 2000) != 0) {
    return -1;
  }
  return cap.count;
}

bool ESP_NOW_Class::begin(const uint8_t *pmk) {
  if (_has_begun) {
    return true;
  }
  if (!g_link.started()) {
    log_e("ESP_NOW.setLink(channel) must be called before begin()");
    return false;
  }

  memset(_peers, 0, sizeof(_peers));

  // Quiet echo so responses parse cleanly (ignore result: ATE0 always OKs).
  g_link.command("ATE0");

  // Clean slate: drop any state left from a previous host session. Harmless
  // no-op right after setLink()'s hardware reset; on boards without the reset
  // pins it clears stale peers/keys so begin() always starts fresh.
  g_link.command("AT+ENDEINIT");

  char cmd[64];
  snprintf(cmd, sizeof(cmd), "AT+ENINIT=%u", (unsigned)g_link.channel());
  if (g_link.command(cmd) != 0) {
    log_e("AT+ENINIT failed");
    return false;
  }

  if (pmk) {
    if (command_hex("AT+ENPMK=", pmk, 16) != 0) {
      log_e("AT+ENPMK failed");
      return false;
    }
  }

  _version = 1;
  VerCap v = {0, false};
  g_link.command("AT+ENVER?", cap_ver, &v);
  if (v.got && v.espnow_ver) {
    _version = v.espnow_ver;
  }
  // Single-frame AT payload cap; matches ESP-NOW v1 (ESP_NOW_MAX_DATA_LEN).
  _max_data_len = ESP_NOW_MAX_DATA_LEN;

  _has_begun = true;
  return true;
}

bool ESP_NOW_Class::end() {
  if (!_has_begun) {
    return true;
  }
  for (int i = 0; i < ESP_NOW_MAX_TOTAL_PEER_NUM; i++) {
    if (_peers[i] != nullptr) {
      removePeer(*_peers[i]);
    }
  }
  int r = g_link.command("AT+ENDEINIT");
  _has_begun = false;
  memset(_peers, 0, sizeof(_peers));
  return r == 0;
}

int ESP_NOW_Class::getTotalPeerCount() const {
  if (!_has_begun) {
    log_e("ESP-NOW not initialized");
    return -1;
  }
  PeerCount pc;
  if (!query_peer_counts(pc)) {
    return -1;
  }
  return pc.total;
}

int ESP_NOW_Class::getEncryptedPeerCount() const {
  if (!_has_begun) {
    log_e("ESP-NOW not initialized");
    return -1;
  }
  PeerCount pc;
  if (!query_peer_counts(pc)) {
    return -1;
  }
  return pc.enc;
}

int ESP_NOW_Class::getMaxDataLen() const {
  if (_max_data_len == 0) {
    log_e("ESP-NOW not initialized. Call begin() first.");
    return -1;
  }
  return _max_data_len;
}

int ESP_NOW_Class::getVersion() const {
  if (_version == 0) {
    log_e("ESP-NOW not initialized. Call begin() first.");
    return -1;
  }
  return _version;
}

int ESP_NOW_Class::availableForWrite() {
  int available = getMaxDataLen();
  return available < 0 ? 0 : available;
}

// Send-to-all: mapped to an ESP-NOW broadcast (AT+ENBCAST). The AT broadcast
// path carries at most ESPNOW_SINGLE_FRAME_MAX bytes (no fragmentation).
size_t ESP_NOW_Class::write(const uint8_t *data, size_t len) {
  if (!_has_begun) {
    log_e("ESP-NOW not initialized. Call begin() first.");
    return 0;
  }
  if (len == 0) {
    return 0;
  }
  if (len > ESPNOW_SINGLE_FRAME_MAX) {
    len = ESPNOW_SINGLE_FRAME_MAX;
  }

  char prefix[24];
  snprintf(prefix, sizeof(prefix), "AT+ENBCAST=%u,", (unsigned)len);
  return command_hex(prefix, data, len) == 0 ? len : 0;
}

void ESP_NOW_Class::onNewPeer(void (*cb)(const esp_now_recv_info_t *info, const uint8_t *data, int len, void *arg), void *arg) {
  new_cb = cb;
  new_arg = arg;
}

bool ESP_NOW_Class::removePeer(ESP_NOW_Peer &peer) {
  return peer.remove();
}

ESP_NOW_Class ESP_NOW;

/* ---- ESP_NOW_Peer -------------------------------------------------- */

ESP_NOW_Peer::ESP_NOW_Peer(const uint8_t *mac_addr, uint8_t channel, wifi_interface_t iface, const uint8_t *lmk, esp_now_rate_config_t *rate_config) {
  added = false;
  if (mac_addr) {
    memcpy(mac, mac_addr, 6);
  }
  chan = channel;
  ifc = iface;
  encrypt = lmk != nullptr;
  if (encrypt) {
    memcpy(key, lmk, 16);
  }
  if (rate_config) {
    rate = *rate_config;
  } else {
    esp_now_rate_config_t def = DEFAULT_ESPNOW_RATE_CONFIG;
    rate = def;
  }
}

bool ESP_NOW_Peer::add() {
  if (!_has_begun) {
    return false;
  }
  if (added) {
    return true;
  }

  int slot = -1;
  for (int i = 0; i < ESP_NOW_MAX_TOTAL_PEER_NUM; i++) {
    if (_peers[i] == nullptr) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    log_e("Library peer list full");
    return false;
  }

  if (at_addpeer(mac, chan, encrypt, key) != 0) {
    log_e("Failed to add peer " MACSTR, MAC2STR(mac));
    return false;
  }
  _peers[slot] = this;
  added = true;
  return true;
}

bool ESP_NOW_Peer::remove() {
  if (!_has_begun) {
    return false;
  }
  if (!added) {
    return true;
  }
  char mstr[13];
  mac_to_hex(mac, mstr);
  char cmd[40];
  snprintf(cmd, sizeof(cmd), "AT+ENDELPEER=%s", mstr);
  if (g_link.command(cmd) != 0) {
    log_e("Failed to remove peer " MACSTR, MAC2STR(mac));
    return false;
  }
  for (int i = 0; i < ESP_NOW_MAX_TOTAL_PEER_NUM; i++) {
    if (_peers[i] == this) {
      _peers[i] = nullptr;
    }
  }
  added = false;
  return true;
}

const uint8_t *ESP_NOW_Peer::addr() const {
  return mac;
}

bool ESP_NOW_Peer::addr(const uint8_t *mac_addr) {
  if (!_has_begun || !added) {
    memcpy(mac, mac_addr, 6);
    return true;
  }
  log_e("Peer already added; call addr() before add()/begin().");
  return false;
}

uint8_t ESP_NOW_Peer::getChannel() const {
  return chan;
}

bool ESP_NOW_Peer::setChannel(uint8_t channel) {
  chan = channel;
  if (!_has_begun || !added) {
    return true;
  }
  return at_addpeer(mac, chan, encrypt, key) == 0;
}

wifi_interface_t ESP_NOW_Peer::getInterface() const {
  return ifc;
}

bool ESP_NOW_Peer::setInterface(wifi_interface_t iface) {
  // The AT bridge always operates on the co-processor's STA interface; the
  // stored value is kept for source compatibility only.
  ifc = iface;
  return true;
}

bool ESP_NOW_Peer::setRate(const esp_now_rate_config_t *rate_config) {
  if (added && _has_begun) {
    log_e("Cannot set rate on a live peer; call setRate() before add().");
    return false;
  }
  if (rate_config == nullptr) {
    esp_now_rate_config_t def = DEFAULT_ESPNOW_RATE_CONFIG;
    rate = def;
  } else {
    rate = *rate_config;
  }
  return true;
}

esp_now_rate_config_t ESP_NOW_Peer::getRate() const {
  return rate;
}

bool ESP_NOW_Peer::isEncrypted() const {
  return encrypt;
}

bool ESP_NOW_Peer::setKey(const uint8_t *lmk) {
  encrypt = lmk != nullptr;
  if (encrypt) {
    memcpy(key, lmk, 16);
  }
  if (!_has_begun || !added) {
    return true;
  }
  return at_addpeer(mac, chan, encrypt, key) == 0;
}

size_t ESP_NOW_Peer::send(const uint8_t *data, int len) {
  if (!_has_begun || !added) {
    log_e("Peer not added.");
    return 0;
  }
  int maxd = ESP_NOW.getMaxDataLen();
  if (maxd < 0) {
    return 0;
  }
  if (len > maxd) {
    len = maxd;
  }
  if (len <= 0) {
    return 0;
  }

  // Broadcast peer: identical wire path to ESP_NOW.write(); delegate so the
  // AT+ENBCAST framing (and its single-frame cap) lives in exactly one place.
  if (is_broadcast(mac)) {
    return ESP_NOW.write(data, (size_t)len);
  }

  char mstr[13];
  mac_to_hex(mac, mstr);
  char prefix[48];
  if ((size_t)len > ESPNOW_SINGLE_FRAME_MAX) {
    // Larger-than-one-frame unicast: the firmware fragments/reassembles.
    snprintf(prefix, sizeof(prefix), "AT+ENFRAGSEND=%s,%d,", mstr, len);
  } else {
    snprintf(prefix, sizeof(prefix), "AT+ENSEND=%s,%d,", mstr, len);
  }
  return command_hex(prefix, data, (size_t)len) == 0 ? (size_t)len : 0;
}

ESP_NOW_Peer::operator bool() const {
  return added;
}
