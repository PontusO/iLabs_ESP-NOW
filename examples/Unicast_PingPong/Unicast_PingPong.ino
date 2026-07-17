/*
    ESP-NOW Unicast Ping  (RP2040/RP2350 host + ESP32-C6 co-processor)

    Minimal unicast demo exercising the "core path": add a known peer, send a
    unicast frame to it every 2 s, and print delivery status (onSent) and any
    replies (onReceive). Run it on two boards, each pointing PEER_MAC at the
    other's ESP32-C6 STA MAC (read it from ESP_NOW.macAddress()).
*/

#include <Arduino.h>
#include "ESP32_NOW.h"

#define ESPNOW_WIFI_CHANNEL 6

// The OTHER board's ESP32-C6 STA MAC. Change this per board.
uint8_t PEER_MAC[6] = {0xF0, 0xF5, 0xBD, 0x31, 0x9B, 0xB0};

class Peer : public ESP_NOW_Peer {
public:
  Peer(const uint8_t *mac) : ESP_NOW_Peer(mac, ESPNOW_WIFI_CHANNEL, WIFI_IF_STA, nullptr) {}

  void onReceive(const uint8_t *data, size_t len, bool broadcast) {
    Serial.printf("RX from " MACSTR " (%s): ", MAC2STR(addr()), broadcast ? "bcast" : "unicast");
    Serial.write(data, len);
    Serial.println();
  }

  void onSent(bool success) {
    Serial.printf("TX to " MACSTR ": %s\n", MAC2STR(addr()), success ? "delivered" : "no ack");
  }

  // Expose the protected helpers for this simple sketch.
  using ESP_NOW_Peer::add;
  using ESP_NOW_Peer::send;
};

Peer peer(PEER_MAC);
uint32_t seq = 0;

void setup() {
  Serial.begin(115200);

#if defined(ESP_SERIAL_PORT)
  // iLabs Challenger boards: use the variant's ESP32 UART + automatic reset.
  ESP_NOW.setLink(ESPNOW_WIFI_CHANNEL);
#else
  Serial1.begin(115200);
  ESP_NOW.setLink(Serial1, ESPNOW_WIFI_CHANNEL);
#endif

  if (!ESP_NOW.begin() || !peer.add()) {
    Serial.println("ESP-NOW init / peer add failed");
    while (true) {
      delay(1000);
    }
  }

  Serial.println("Unicast ready. My MAC: " + ESP_NOW.macAddress());
}

void loop() {
  char msg[24];
  int n = snprintf(msg, sizeof(msg), "ping %lu", (unsigned long)seq++);
  peer.send((const uint8_t *)msg, n);

  // Wait 2 s while servicing replies and the send-status URC.
  for (uint32_t t = millis(); millis() - t < 2000;) {
    ESP_NOW.poll();
    delay(5);
  }
}
