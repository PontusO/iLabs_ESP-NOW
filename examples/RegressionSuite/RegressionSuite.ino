/*
    iLabs ESP-NOW - Regression Suite  (RP2040/RP2350 host + ESP32-C6)

    A two-board self-test that exercises the whole library surface (and, through
    it, the AT+EN interpreter) and prints a PASS/FAIL report you can diff after
    changing the library or the firmware.

    HOW IT WORKS
    ------------
    Flash this SAME sketch onto TWO boards on the same channel. At boot each
    board discovers the other and roles are assigned automatically by MAC:
      - lower MAC  -> TESTER    : runs the scored sequence, prints the report
      - higher MAC -> RESPONDER : an echo + control server the tester drives
    Watch the TESTER's USB serial (115200) for the report. See extras/REGRESSION.md
    for the recommended hardware rig.

    COVERAGE
    --------
    The scored sequence runs in two phases:

    Phase 1 - raw AT protocol (ESP_NOW.command() passthrough): positive OK
            paths for the query/exec commands, and negative paths that assert
            the firmware rejects malformed input with the exact spec'd result
            (+ENERR:<n> code, plain ERROR, or timeout) - bad grammar, wrong
            command TYPE, out-of-range channel, malformed MAC/PMK/LMK, and
            over-length / non-hex payloads on SEND/BCAST/FRAGSEND. This is the
            drift tripwire against the AT+EN Command Set Spec v0.1.

    Phase 2 - library API:
      Local:  setLink, begin, begin(pmk), end, macAddress, getVersion,
              getMaxDataLen, availableForWrite, discover, onReset, wasReset,
              getTotalPeerCount, getEncryptedPeerCount, removePeer.
      Peer:   ctor, add, remove, send, addr/addr(set), get/setChannel,
              get/setInterface, get/setRate, isEncrypted, setKey, operator bool,
              onReceive, onSent.
      Neg:    zero/negative-length send short-circuits, add()/removePeer()
              idempotency + count accounting, an out-of-range-channel add()
              that must fail, and discover() argument guards.
      OTA:    unicast (single-frame + fragmented), broadcast (write + write(byte)),
              onNewPeer, and an encrypted unicast round-trip (per-peer LMK).

    NOTE: fragmented RECEIVE beyond ~290 B needs a bigger line buffer; build with
    -DILABS_ESPNOW_LINE_MAX=2048 to raise the frag test payloads.
*/

#include <Arduino.h>
#include "ESP32_NOW.h"

#define ESPNOW_WIFI_CHANNEL 6
#define RT_TIMEOUT_MS       1500          // per round-trip wait
#define RESP_IDLE_DROP_MS   8000          // responder drops a silent tester

// Shared per-peer key for the encrypted round-trip (both boards, compile-time).
static const uint8_t SHARED_LMK[16] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                                       0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F};
// A PMK just to exercise begin(pmk) / AT+ENPMK.
static const uint8_t SHARED_PMK[16] = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
                                       0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F};
// A throwaway MAC + LMK for the local encrypted-peer table test.
static const uint8_t DUMMY_MAC[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
static const uint8_t DUMMY_LMK[16] = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
                                      0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF};

// Control opcodes (first payload byte, tester -> responder).
#define OP_HELLO   0xE0   // (re)register me fresh, reply ack
#define OP_ECHO    0xE1   // echo the rest back, prefixed with a flags byte
#define OP_ENCRYPT 0xE2   // switch our link to encrypted (SHARED_LMK)
#define OP_PLAIN   0xE3   // switch our link back to unencrypted
#define ACK_BYTE   0xAC   // responder reply body for control ops

// Responder reply layout: [flags][body...], flags bit0 = frame was broadcast.

/* ---- shared helpers ------------------------------------------------ */

static String macStr(const uint8_t *m) {
  char b[13];
  snprintf(b, sizeof(b), "%02X%02X%02X%02X%02X%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return String(b);
}

static void pollFor(uint32_t ms) {
  uint32_t t = millis();
  while (millis() - t < ms) {
    ESP_NOW.poll();
  }
}

/* ---- TESTER: received-frame capture -------------------------------- */

volatile bool g_rx = false;
uint8_t g_rx_buf[600];
int g_rx_len = 0;
bool g_rx_bcast = false;
volatile bool g_sent = false;
bool g_sent_ok = false;

class TesterPeer : public ESP_NOW_Peer {
public:
  TesterPeer(const uint8_t *mac) : ESP_NOW_Peer(mac, ESPNOW_WIFI_CHANNEL, WIFI_IF_STA, nullptr) {}
  void onReceive(const uint8_t *data, size_t len, bool broadcast) {
    g_rx_len = (int)min(len, sizeof(g_rx_buf));
    memcpy(g_rx_buf, data, g_rx_len);
    g_rx_bcast = broadcast;
    g_rx = true;
  }
  void onSent(bool success) {
    g_sent_ok = success;
    g_sent = true;
  }
  using ESP_NOW_Peer::add;
  using ESP_NOW_Peer::remove;
  using ESP_NOW_Peer::send;
};

// The tester's peer (a global so the helper functions below don't take a
// TesterPeer* parameter, which the Arduino .ino auto-prototyper would hoist
// above the class definition).
TesterPeer *g_tp = nullptr;

/* ---- RESPONDER: echo + control server ------------------------------ */

uint32_t g_resp_last_rx = 0;

class RespPeer : public ESP_NOW_Peer {
public:
  RespPeer(const uint8_t *mac) : ESP_NOW_Peer(mac, ESPNOW_WIFI_CHANNEL, WIFI_IF_STA, nullptr) {}

  void onReceive(const uint8_t *data, size_t len, bool broadcast) {
    g_resp_last_rx = millis();
    if (len < 1) {
      return;
    }
    uint8_t op = data[0];
    uint8_t reply[600];
    reply[0] = broadcast ? 0x01 : 0x00;  // flags

    if (op == OP_ECHO) {
      int body = (int)min(len - 1, sizeof(reply) - 1);
      memcpy(reply + 1, data + 1, body);
      send(reply, body + 1);
    } else if (op == OP_HELLO) {
      reply[1] = ACK_BYTE;
      send(reply, 2);
    } else if (op == OP_ENCRYPT || op == OP_PLAIN) {
      // Re-key our side of the link straight from the callback. setKey() issues
      // AT+ENADDPEER, i.e. it re-enters the AT transport - the library defers
      // that safely because we're inside a receive callback. (No ack: the
      // tester waits a fixed window instead.)
      setKey(op == OP_ENCRYPT ? SHARED_LMK : nullptr);
    }
  }

  using ESP_NOW_Peer::add;
  using ESP_NOW_Peer::remove;
  using ESP_NOW_Peer::send;
  using ESP_NOW_Peer::setKey;
};

RespPeer *g_tester_peer = nullptr;

// Register (or re-register) the tester when it first talks to us.
void onNewPeerCb(const esp_now_recv_info_t *info, const uint8_t *data, int len, void *arg) {
  (void)data;
  (void)len;
  (void)arg;
  if (g_tester_peer) {
    return;  // already have one (single-tester rig)
  }
  g_tester_peer = new RespPeer(info->src_addr);
  if (!g_tester_peer->add()) {
    delete g_tester_peer;
    g_tester_peer = nullptr;
    return;
  }
  g_resp_last_rx = millis();
  Serial.printf("RESPONDER: registered tester %s\n", macStr(info->src_addr).c_str());
}

/* ---- TESTER: scoring ----------------------------------------------- */

int g_pass = 0, g_fail = 0;

void check(const char *name, bool cond) {
  Serial.printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name);
  if (cond) {
    g_pass++;
  } else {
    g_fail++;
  }
}

// Send [op]+payload (unicast or broadcast) and wait for the responder reply.
bool rpc(uint8_t op, const uint8_t *payload, int plen, bool broadcast) {
  uint8_t frame[600];
  frame[0] = op;
  if (plen > 0) {
    memcpy(frame + 1, payload, plen);
  }
  g_rx = false;
  g_sent = false;
  if (broadcast) {
    ESP_NOW.write(frame, plen + 1);
  } else {
    g_tp->send(frame, plen + 1);
  }
  uint32_t t0 = millis();
  while (!g_rx && millis() - t0 < RT_TIMEOUT_MS) {
    ESP_NOW.poll();
  }
  return g_rx;
}

// Send a 1-byte control opcode (no reply expected).
void sendCtrl(uint8_t op) {
  uint8_t f = op;
  g_tp->send(&f, 1);
}

// Echo round-trip: reply must be [flags][payload], flags bcast bit as expected.
// Prints a one-line diagnosis on failure (blind-debug aid).
bool echoOk(const uint8_t *payload, int plen, bool broadcast) {
  bool gotReply = false;
  int lastLen = -1, lastFlag = -1, mism = -1;
  for (int attempt = 0; attempt < 3; attempt++) {
    if (rpc(OP_ECHO, payload, plen, broadcast)) {
      gotReply = true;
      lastLen = g_rx_len;
      lastFlag = g_rx_buf[0];
      bool bcast_bit = (g_rx_buf[0] & 0x01) != 0;
      mism = (g_rx_len == plen + 1) ? memcmp(g_rx_buf + 1, payload, plen) : 999;
      if (g_rx_len == plen + 1 && bcast_bit == broadcast && mism == 0) {
        return true;
      }
    }
  }
  Serial.printf("      diag: gotReply=%d len=%d(exp %d) flag=0x%02X exp_bcast=%d payloadMism=%d\n", gotReply, lastLen,
                plen + 1, lastFlag < 0 ? 0 : lastFlag, broadcast, mism);
  return false;
}

/* ---- PHASE 1: raw AT-protocol tests -------------------------------- *
 * These drive the AT+EN firmware directly through ESP_NOW.command(), the
 * library's raw passthrough, and assert the exact terminal result code:
 *     0  = OK, >0 = the +ENERR:<n> code, -1 = plain ERROR, -2 = timeout.
 * They pin the firmware's spec'd accept/reject contract so a drift in its
 * argument validation shows up as a FAIL. Nothing here goes over the air or
 * changes the link/radio config: every negative case is rejected before any
 * side effect, and the one add/delete pair nets back to zero peers.        */

static char g_at_line[96];
static bool g_at_got;

static void atCapCb(const char *line, void *) {
  strncpy(g_at_line, line, sizeof(g_at_line) - 1);
  g_at_line[sizeof(g_at_line) - 1] = '\0';
  g_at_got = true;
}

// Result code of a raw AT command (see ESP_NOW.command() for the mapping).
static int atCmd(const char *cmd) {
  return ESP_NOW.command(cmd);
}

// True iff `cmd` returns OK *and* emits an intermediate line starting `prefix`.
static bool atCap(const char *cmd, const char *prefix) {
  g_at_got = false;
  g_at_line[0] = '\0';
  int r = ESP_NOW.command(cmd, atCapCb, nullptr);
  return r == 0 && g_at_got && strncmp(g_at_line, prefix, strlen(prefix)) == 0;
}

void runRawATTests() {
  Serial.println("\n--- Phase 1: raw AT protocol (positive + negative) ---");

  // === Positive: well-formed commands must answer OK ===
  check("[AT+] bare AT -> OK", atCmd("AT") == 0);
  check("[AT+] ENVER? -> OK", atCmd("AT+ENVER?") == 0);
  check("[AT+] ENSTATE? -> OK", atCmd("AT+ENSTATE?") == 0);
  check("[AT+] ENSTATS? -> OK", atCmd("AT+ENSTATS?") == 0);
  check("[AT+] ENCHANNEL? -> OK", atCmd("AT+ENCHANNEL?") == 0);
  check("[AT+] ENFLOW? -> OK", atCmd("AT+ENFLOW?") == 0);
  check("[AT+] ENDISCOVERABLE? -> OK", atCmd("AT+ENDISCOVERABLE?") == 0);
  check("[AT+] CGMI -> OK", atCmd("AT+CGMI") == 0);
  check("[AT+] CGMM -> OK", atCmd("AT+CGMM") == 0);
  check("[AT+] CGMR -> OK", atCmd("AT+CGMR") == 0);

  // Positive with response-shape capture.
  check("[AT+] ENVER? emits +ENVER:", atCap("AT+ENVER?", "+ENVER:"));
  check("[AT+] ENMAC? emits +ENMAC:", atCap("AT+ENMAC?", "+ENMAC:"));
  check("[AT+] ENSTATE? emits +ENSTATE:", atCap("AT+ENSTATE?", "+ENSTATE:"));

  // Positive add/delete round-trip (channel matches begin()'s; nets to 0 peers).
  check("[AT+] ADDPEER valid -> OK", atCmd("AT+ENADDPEER=020000000041,6,0") == 0);
  check("[AT+] LISTPEER? -> OK", atCmd("AT+ENLISTPEER?") == 0);
  check("[AT+] DELPEER valid -> OK", atCmd("AT+ENDELPEER=020000000041") == 0);

  // === Negative: malformed input must be rejected with the spec'd code ===

  // -- parser / grammar --
  check("[AT-] unknown command -> +ENERR:8", atCmd("AT+ENBOGUS") == 8);
  check("[AT-] non-AT line -> ERROR", atCmd("HELLO") == -1);
  check("[AT-] trailing char after '?' -> ERROR", atCmd("AT+ENVER?X") == -1);
  check("[AT-] SET on query-only ENVER -> ERROR", atCmd("AT+ENVER=1") == -1);
  check("[AT-] QUERY on exec-only CGMI -> ERROR", atCmd("AT+CGMI?") == -1);

  // -- channel range (plain ERROR, no code) --
  check("[AT-] ENINIT channel 0 -> ERROR", atCmd("AT+ENINIT=0") == -1);
  check("[AT-] ENINIT channel 15 -> ERROR", atCmd("AT+ENINIT=15") == -1);
  check("[AT-] ENCHANNEL 15 -> ERROR", atCmd("AT+ENCHANNEL=15") == -1);

  // -- ADDPEER argument validation --
  check("[AT-] ADDPEER too few args -> ERROR", atCmd("AT+ENADDPEER=020000000041,6") == -1);
  check("[AT-] ADDPEER bad MAC -> ERROR", atCmd("AT+ENADDPEER=ZZ0000000041,6,0") == -1);
  check("[AT-] ADDPEER short MAC -> ERROR", atCmd("AT+ENADDPEER=0200000041,6,0") == -1);
  check("[AT-] ADDPEER encrypt>1 -> ERROR", atCmd("AT+ENADDPEER=020000000041,6,2") == -1);
  check("[AT-] ADDPEER bad-length LMK -> +ENERR:6", atCmd("AT+ENADDPEER=020000000041,6,1,0011223344") == 6);

  // -- key commands (all malformed forms -> +ENERR:6) --
  check("[AT-] PMK short key -> +ENERR:6", atCmd("AT+ENPMK=00112233") == 6);
  check("[AT-] PMK non-hex key -> +ENERR:6", atCmd("AT+ENPMK=GG112233445566778899AABBCCDDEEFF") == 6);
  check("[AT-] LMK too few args -> +ENERR:6", atCmd("AT+ENLMK=020000000041") == 6);

  // -- data path length / hex validation --
  check("[AT-] SEND len 0 -> +ENERR:4", atCmd("AT+ENSEND=020000000041,0,00") == 4);
  check("[AT-] SEND len>248 -> +ENERR:4", atCmd("AT+ENSEND=020000000041,249,AB") == 4);
  check("[AT-] SEND hex len mismatch -> ERROR", atCmd("AT+ENSEND=020000000041,4,DEADBE") == -1);
  check("[AT-] SEND non-hex payload -> ERROR", atCmd("AT+ENSEND=020000000041,2,ZZZZ") == -1);
  check("[AT-] BCAST len>248 -> +ENERR:4", atCmd("AT+ENBCAST=249,AB") == 4);
  check("[AT-] FRAGSEND len>4096 -> +ENERR:4", atCmd("AT+ENFRAGSEND=020000000041,4097,AB") == 4);

  // -- discover window / misc range guards (plain ERROR) --
  check("[AT-] DISCOVER window <50 -> ERROR", atCmd("AT+ENDISCOVER=49") == -1);
  check("[AT-] DISCOVER window >30000 -> ERROR", atCmd("AT+ENDISCOVER=30001") == -1);
  check("[AT-] FLOW mode>3 -> ERROR", atCmd("AT+ENFLOW=4") == -1);
  check("[AT-] RATE index>42 -> ERROR", atCmd("AT+ENRATE=43") == -1);
  check("[AT-] BAUD invalid value -> ERROR", atCmd("AT+ENBAUD=12345") == -1);
}

void runTests(const uint8_t *peerMac) {
  Serial.println("\n===== iLabs ESP-NOW regression suite =====");

  runRawATTests();  // Phase 1: raw AT-protocol contract

  Serial.println("\n--- Phase 2: library API (contract + OTA) ---");

  // --- Local getters -------------------------------------------------
  String mac = ESP_NOW.macAddress();
  check("macAddress() is 12 hex chars", mac.length() == 12);
  check("getVersion() >= 1", ESP_NOW.getVersion() >= 1);
  check("getMaxDataLen() == 250", ESP_NOW.getMaxDataLen() == 250);
  // write() is a single broadcast frame, so availableForWrite() reports the
  // 248-byte single-frame ceiling, not the 250-byte unicast max.
  check("availableForWrite() == 248", ESP_NOW.availableForWrite() == 248);
  check("wasReset() is false at start", ESP_NOW.wasReset() == false);
  ESP_NOW.onReset([](void *) {}, nullptr);  // exercise the setter
  check("getTotalPeerCount() == 0 initially", ESP_NOW.getTotalPeerCount() == 0);

  // --- Negative / boundary: library contract ------------------------
  // These stay count-neutral so the assertions above/below still hold.
  int base = ESP_NOW.getTotalPeerCount();  // 0 here
  const uint8_t nbuf[4] = {0xAA, 0xBB, 0xCC, 0xDD};
  static const uint8_t NEG_MAC[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x7F};

  // Zero/negative-length sends short-circuit in the library (no frame sent).
  check("write(_, 0) returns 0", ESP_NOW.write(nbuf, 0) == 0);
  check("begin() again is idempotent", ESP_NOW.begin(SHARED_PMK) == true);

  TesterPeer *np = new TesterPeer(NEG_MAC);
  check("send() before add() returns 0", np->send(nbuf, sizeof(nbuf)) == 0);
  check("send(_, 0) returns 0", np->send(nbuf, 0) == 0);
  check("send(_, -1) returns 0", np->send(nbuf, -1) == 0);

  // The library does NOT validate channel client-side; the firmware must, so
  // add() with an out-of-range channel has to fail (catches firmware drift).
  np->setChannel(200);
  check("add() with channel 200 -> false (firmware rejects)", np->add() == false);
  check("failed add() consumed no peer slot", ESP_NOW.getTotalPeerCount() == base);
  np->setChannel(ESPNOW_WIFI_CHANNEL);

  // add / count / idempotency / remove accounting.
  check("add() valid -> true", np->add());
  check("peer count +1 after add", ESP_NOW.getTotalPeerCount() == base + 1);
  check("add() again is idempotent (still true)", np->add());
  check("idempotent add did not double-count", ESP_NOW.getTotalPeerCount() == base + 1);
  check("removePeer() -> true", ESP_NOW.removePeer(*np));
  check("peer count back to base", ESP_NOW.getTotalPeerCount() == base);
  check("removePeer() of already-removed -> true (no-op)", ESP_NOW.removePeer(*np));
  check("no-op removePeer left count at base", ESP_NOW.getTotalPeerCount() == base);
  delete np;

  // discover() client-side argument guards return -1 without touching the link.
  ESP_NOW_Found ntmp[2];
  check("discover(nullptr, ...) -> -1", ESP_NOW.discover(nullptr, 2, 100) == -1);
  check("discover(_, 0, _) -> -1", ESP_NOW.discover(ntmp, 0, 100) == -1);

  // --- end() + begin(pmk) cycle -------------------------------------
  check("end() returns true", ESP_NOW.end() == true);
  check("begin(pmk) returns true", ESP_NOW.begin(SHARED_PMK) == true);

  // --- Peer object local state (before add) -------------------------
  g_tp = new TesterPeer(DUMMY_MAC);
  check("new peer: operator bool() == false", (bool)(*g_tp) == false);
  check("addr() matches ctor mac", memcmp(g_tp->addr(), DUMMY_MAC, 6) == 0);
  check("addr(set) updates the mac", g_tp->addr(peerMac) && memcmp(g_tp->addr(), peerMac, 6) == 0);
  g_tp->setChannel(11);
  check("set/getChannel() round-trips", g_tp->getChannel() == 11);
  g_tp->setChannel(ESPNOW_WIFI_CHANNEL);
  g_tp->setInterface(WIFI_IF_AP);
  check("set/getInterface() round-trips", g_tp->getInterface() == WIFI_IF_AP);
  g_tp->setInterface(WIFI_IF_STA);
  esp_now_rate_config_t rc = g_tp->getRate();
  rc.dcm = true;
  check("setRate() accepts a config", g_tp->setRate(&rc) == true);
  check("getRate() reflects it", g_tp->getRate().dcm == true);
  check("isEncrypted() false by default", g_tp->isEncrypted() == false);
  check("setKey(lmk) -> isEncrypted() true", g_tp->setKey(DUMMY_LMK) && g_tp->isEncrypted());
  check("setKey(nullptr) -> isEncrypted() false", g_tp->setKey(nullptr) && !g_tp->isEncrypted());

  // --- add / counts / remove ----------------------------------------
  check("add() peer returns true", g_tp->add());
  check("operator bool() == true after add", (bool)(*g_tp) == true);
  check("getTotalPeerCount() == 1", ESP_NOW.getTotalPeerCount() == 1);

  // Encrypted peer table entry (local; explicit LMK, no OTA needed).
  TesterPeer *enc = new TesterPeer(DUMMY_MAC);
  enc->setKey(DUMMY_LMK);
  check("encrypted add() returns true", enc->add());
  check("getEncryptedPeerCount() == 1", ESP_NOW.getEncryptedPeerCount() == 1);
  check("removePeer(encrypted) returns true", ESP_NOW.removePeer(*enc));
  check("getEncryptedPeerCount() == 0", ESP_NOW.getEncryptedPeerCount() == 0);
  delete enc;

  // --- discover() ----------------------------------------------------
  ESP_NOW_Found found[8];
  int nf = ESP_NOW.discover(found, 8, 1000);
  bool sawPeer = false;
  for (int i = 0; i < nf; i++) {
    if (memcmp(found[i].mac, peerMac, 6) == 0) {
      sawPeer = true;
    }
  }
  check("discover() finds the responder", sawPeer);

  // --- OTA: warm up the link (responder registers us) ---------------
  bool warm = false;
  for (int i = 0; i < 20 && !warm; i++) {
    warm = rpc(OP_HELLO, nullptr, 0, false);
  }
  check("link warm-up handshake", warm);
  check("onSent() fired (delivery status)", g_sent && g_sent_ok);

  // --- OTA: unicast single-frame echo -------------------------------
  const uint8_t small[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
  check("unicast single-frame round-trip", echoOk(small, sizeof(small), false));

  // --- OTA: unicast fragmented echo (payload > 248 B) ---------------
  // 249 payload + 1 opcode = a 250 B frame -> fragmented (>248), and still
  // within getMaxDataLen()==250 so send() doesn't clamp off the last byte.
  static uint8_t big[249];
  for (int i = 0; i < (int)sizeof(big); i++) {
    big[i] = (uint8_t)(i * 7 + 1);
  }
  check("unicast fragmented round-trip (250 B frame)", echoOk(big, sizeof(big), false));

  // --- OTA: broadcast ------------------------------------------------
  const uint8_t bc[6] = {0xB0, 0xCA, 0x57, 0x11, 0x22, 0x33};
  check("broadcast round-trip (bcast flag seen)", echoOk(bc, sizeof(bc), true));
  ESP_NOW.write((uint8_t)'X');  // exercise write(uint8_t) (single-byte broadcast)
  check("write(uint8_t) did not fault", true);

  // --- OTA: encrypted unicast round-trip (per-peer LMK) -------------
  sendCtrl(OP_ENCRYPT);      // responder switches its side (in its loop())
  pollFor(600);              // give it time to re-key
  g_tp->setKey(SHARED_LMK);  // our side encrypted too
  bool encOk = echoOk(small, sizeof(small), false);
  check("encrypted unicast round-trip", encOk);
  // Tear back down so re-runs start unencrypted.
  sendCtrl(OP_PLAIN);
  pollFor(600);
  g_tp->setKey(nullptr);
  check("back to unencrypted after teardown", echoOk(small, sizeof(small), false));

  // --- removePeer / counts ------------------------------------------
  int before = ESP_NOW.getTotalPeerCount();
  check("removePeer(responder) returns true", ESP_NOW.removePeer(*g_tp));
  check("peer count dropped after removePeer", ESP_NOW.getTotalPeerCount() == before - 1);
  delete g_tp;

  // --- Summary -------------------------------------------------------
  Serial.printf("\n===== RESULT: %d passed, %d failed =====\n", g_pass, g_fail);
  Serial.println(g_fail == 0 ? ">>> ALL TESTS PASSED <<<" : ">>> REGRESSIONS DETECTED <<<");
}

/* ---- roles & entry points ------------------------------------------ */

enum { ROLE_UNKNOWN, ROLE_TESTER, ROLE_RESPONDER };
int g_role = ROLE_UNKNOWN;

void setup() {
  Serial.begin(115200);
  uint32_t t = millis();
  while (!Serial && millis() - t < 3000) {
  }

  ESP_NOW.setLink(ESPNOW_WIFI_CHANNEL);
  // Both boards share the same PMK so the encrypted round-trip interoperates
  // (ESP-NOW needs a matching PMK, not just a matching per-peer LMK). The
  // tester re-begins with the same PMK during its scored end()/begin() test.
  if (!ESP_NOW.begin(SHARED_PMK)) {
    Serial.println("FATAL: ESP_NOW.begin() failed");
    while (true) {
      delay(1000);
    }
  }
  String myMac = ESP_NOW.macAddress();
  Serial.println("\nRegression node. My MAC: " + myMac);

  // Discover the partner and assign roles by MAC (lower MAC = tester).
  ESP_NOW_Found found[8];
  uint8_t peerMac[6];
  bool have = false;
  Serial.println("Discovering partner...");
  while (!have) {
    int n = ESP_NOW.discover(found, 8, 1000);
    if (n > 0) {
      memcpy(peerMac, found[0].mac, 6);
      have = true;
    }
  }
  String peerMacS = macStr(peerMac);
  g_role = (myMac < peerMacS) ? ROLE_TESTER : ROLE_RESPONDER;
  Serial.printf("Partner %s -> role: %s\n", peerMacS.c_str(), g_role == ROLE_TESTER ? "TESTER" : "RESPONDER");

  if (g_role == ROLE_TESTER) {
    runTests(peerMac);  // one-shot scored sequence
  } else {
    ESP_NOW.onNewPeer(onNewPeerCb, nullptr);
    Serial.println("RESPONDER ready (echo + control server).");
  }
}

void loop() {
  ESP_NOW.poll();

  if (g_role == ROLE_RESPONDER && g_tester_peer && millis() - g_resp_last_rx > RESP_IDLE_DROP_MS) {
    // Tester went quiet (finished, reset, or crashed) - forget it so a fresh
    // run re-registers cleanly (also recovers from a stale encrypted peer).
    Serial.println("RESPONDER: tester idle, dropping peer");
    ESP_NOW.removePeer(*g_tester_peer);
    delete g_tester_peer;
    g_tester_peer = nullptr;
  }
}
