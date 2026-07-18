/*
 * ESP32_NOW.h - Arduino ESP-NOW class API for RP2040/RP2350 hosts.
 *
 * Public interface is source-compatible with the arduino-esp32 "ESP_NOW"
 * library (global ESP_NOW instance + ESP_NOW_Peer subclassing), so sketches
 * written against that API build unchanged. The radio runs on an ESP32-C6/C3
 * flashed with the iLabs AT+EN ESP-NOW interpreter; this library talks to it
 * over a Serial link.
 *
 * Two things differ from the ESP32-native library, by necessity of the
 * host/co-processor split (see the added members at the end of ESP_NOW_Class):
 *   - call ESP_NOW.setLink(channel) once before ESP_NOW.begin(), in place of
 *     the ESP32 WiFi.mode()/WiFi.setChannel() bring-up (the library uses the
 *     board variant's ESP_SERIAL_PORT; the sketch never names a serial port);
 *   - call ESP_NOW.poll() from loop() so received frames are dispatched to
 *     the peer onReceive() callbacks.
 */

#pragma once

#include "ilabs_espnow_compat.h"

// clang-format off
#define DEFAULT_ESPNOW_RATE_CONFIG { \
  .phymode = WIFI_PHY_MODE_11G,      \
  .rate = WIFI_PHY_RATE_1M_L,        \
  .ersu = false,                     \
  .dcm = false                       \
}
// clang-format on

/* Default baud for the AT link. Matches the AT firmware's boot baud
 * (EN_UART_BAUD). Used by the auto setLink(channel) overload. */
#ifndef ILABS_ESPNOW_LINK_BAUD
#define ILABS_ESPNOW_LINK_BAUD 115200
#endif

// A responder found by ESP_NOW.discover() (iLabs AT+ENDISCOVER extension).
struct ESP_NOW_Found {
  uint8_t mac[6];  // the responder's STA MAC
  int     rssi;    // RSSI measured by THIS device (0 if unavailable)
};

class ESP_NOW_Peer;  //forward declaration for friend function

class ESP_NOW_Class : public Print {
public:
  const uint8_t BROADCAST_ADDR[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

  ESP_NOW_Class();
  ~ESP_NOW_Class();

  bool begin(const uint8_t *pmk = nullptr /* 16 bytes */);
  bool end();

  int getTotalPeerCount() const;
  int getEncryptedPeerCount() const;
  int getMaxDataLen() const;
  int getVersion() const;

  int availableForWrite();

  // You can directly send data to all peers without broadcasting using ESP_NOW.write(data, len)
  size_t write(const uint8_t *data, size_t len);
  size_t write(uint8_t data) {
    return write(&data, 1);
  }

  void onNewPeer(void (*cb)(const esp_now_recv_info_t *info, const uint8_t *data, int len, void *arg), void *arg);
  bool removePeer(ESP_NOW_Peer &peer);

  /* ---- iLabs host-bridge extensions (not in the ESP32-native API) ---- *
   * Additive only, so upstream sketches remain source-compatible.        */

  // Bring up the link to the ESP32 co-processor and pick the ESP-NOW channel.
  // Call once in setup() before begin(); replaces the ESP32 WiFi bring-up.
  //
  // The UART is the one the board variant wires to the ESP32 (ESP_SERIAL_PORT)
  // - the sketch never names a serial port. When the variant also defines the
  // reset pins (PIN_ESP_MODE / PIN_ESP_RST), this performs an automatic
  // hardware reset of the ESP32 into run mode and waits for its +ENREADY,
  // giving a deterministic clean boot every time.
  //
  // Requires a board variant that defines ESP_SERIAL_PORT (e.g. an iLabs
  // Challenger WiFi/WiFi6 board).
  void setLink(uint8_t channel);

  // Pump the AT link: read and dispatch pending +ENRECV / send-status URCs.
  // Call frequently from loop() (received frames arrive via peer onReceive).
  void poll();

  // This device's (the ESP32 co-processor's) STA MAC, "AABBCCDDEEFF".
  // Valid after setLink(); works before begin().
  String macAddress();

  // Discover other iLabs ESP-NOW devices on the current channel: broadcast a
  // probe and collect responders for `timeout_ms` (0 = firmware default, ~1s;
  // otherwise 50..30000). Fills up to `max` entries of `out` and returns the
  // count found (0 if none) or -1 on error. Blocks for the collection window.
  // iLabs AT+ENDISCOVER extension - not part of the ESP32-native API.
  int discover(ESP_NOW_Found *out, int max, uint32_t timeout_ms = 0);

  // Register a callback fired when the co-processor unexpectedly reboots (its
  // +ENREADY boot marker is seen during operation). The ESP32 loses its peers
  // and keys on reset; the simplest recovery is to reboot the host
  // (rp2040.reboot()), which re-runs the hardware reset + provisioning.
  void onReset(void (*cb)(void *arg), void *arg = nullptr);

  // True (once) if the co-processor rebooted since the last call; clears on read.
  bool wasReset();

  // Send a raw AT command line to the co-processor and wait for its terminal
  // response. For diagnostics and for regression-testing the AT+EN firmware
  // against its spec - normal sketches use the typed API above instead.
  //
  // Returns the transport result code:
  //     0  on OK,
  //    >0  the +ENERR:<n> code on a coded error,
  //    -1  on a plain ERROR,
  //    -2  on timeout / link not started.
  // Any intermediate result line (e.g. "+ENVER:1.0.0,0", "+ENMAC:...") is
  // passed to onLine(line, arg) if given. `cmd` is sent verbatim (no CRLF
  // needed - the transport appends it). iLabs extension, not in the ESP32 API.
  int command(const char *cmd,
              void (*onLine)(const char *line, void *arg) = nullptr,
              void *arg = nullptr, uint32_t timeout_ms = 0);

  // The singleton holds no per-instance state: begin()-negotiated values
  // (version, max data length) and the peer table live in ESP32_NOW.cpp.
};

class ESP_NOW_Peer {
private:
  uint8_t mac[6];
  uint8_t chan;
  wifi_interface_t ifc;
  esp_now_rate_config_t rate;
  bool encrypt;
  uint8_t key[16];

protected:
  bool added;
  bool add();
  bool remove();
  size_t send(const uint8_t *data, int len);

  ESP_NOW_Peer(
    const uint8_t *mac_addr, uint8_t channel = 0, wifi_interface_t iface = WIFI_IF_AP, const uint8_t *lmk = nullptr,
    esp_now_rate_config_t *rate_config = nullptr
  );

public:
  virtual ~ESP_NOW_Peer() {}

  const uint8_t *addr() const;
  bool addr(const uint8_t *mac_addr);

  uint8_t getChannel() const;
  bool setChannel(uint8_t channel);

  wifi_interface_t getInterface() const;
  bool setInterface(wifi_interface_t iface);

  bool setRate(const esp_now_rate_config_t *rate_config);
  esp_now_rate_config_t getRate() const;

  bool isEncrypted() const;
  bool setKey(const uint8_t *lmk);

  operator bool() const;

  //optional callbacks to be implemented by the upper class
  virtual void onReceive(const uint8_t *data, size_t len, bool broadcast) {
    log_i("Received %lu bytes from " MACSTR " %s", (unsigned long)len, MAC2STR(mac), broadcast ? "(broadcast)" : "");
  }

  virtual void onSent(bool success) {
    log_i("Message transmission to peer " MACSTR " %s", MAC2STR(mac), success ? "successful" : "failed");
  }

  friend bool ESP_NOW_Class::removePeer(ESP_NOW_Peer &);
  friend class ESP_NOW_Class;
};

#if !defined(NO_GLOBAL_INSTANCES) && !defined(NO_GLOBAL_ESP_NOW)
extern ESP_NOW_Class ESP_NOW;
#endif
