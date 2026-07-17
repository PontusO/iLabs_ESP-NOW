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
 *   - call ESP_NOW.setLink(Serial1, channel) once before ESP_NOW.begin(),
 *     in place of the ESP32 WiFi.mode()/WiFi.setChannel() bring-up;
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

  // Bind the AT link to the ESP32 co-processor and pick the ESP-NOW
  // channel. Call once in setup() before begin(). `serial` must already be
  // begin()'d by the sketch. Replaces the ESP32 WiFi.mode()/setChannel().
  void setLink(Stream &serial, uint8_t channel);

  // Pump the AT link: read and dispatch pending +ENRECV / send-status URCs.
  // Call frequently from loop() (received frames arrive via peer onReceive).
  void poll();

  // This device's (the ESP32 co-processor's) STA MAC, "AABBCCDDEEFF".
  // Valid after setLink(); works before begin().
  String macAddress();

protected:
  size_t max_data_len;
  uint32_t version;
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
