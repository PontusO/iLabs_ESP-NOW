/*
    ESP-NOW Discovery Ping  (RP2040/RP2350 host + ESP32-C6 co-processor)

    Like Unicast_PingPong, but instead of hard-coding the other board's MAC it
    finds it at runtime with ESP_NOW.discover() (the iLabs AT+ENDISCOVER
    extension). Flash the SAME sketch onto two boards on the same channel: each
    scans, discovers the other, pairs with it, and they ping-pong.

    discover() broadcasts a probe and returns every iLabs ESP-NOW device that
    answers on the current channel, with the RSSI this board measured - so with
    more than two boards around you could pick by signal strength, filter, etc.
*/

#include <Arduino.h>
#include "ESP32_NOW.h"

#define ESPNOW_WIFI_CHANNEL 6
#define MAX_FOUND           8

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

  using ESP_NOW_Peer::add;
  using ESP_NOW_Peer::send;
};

Peer *peer = nullptr;
uint32_t seq = 0;

void setup() {
  Serial.begin(115200);

  // The library uses the board variant's ESP32 UART and resets it automatically.
  ESP_NOW.setLink(ESPNOW_WIFI_CHANNEL);

  if (!ESP_NOW.begin()) {
    Serial.println("ESP-NOW init failed");
    while (true) {
      delay(1000);
    }
  }
  Serial.println("Discovery Ping. My MAC: " + ESP_NOW.macAddress());

  // Find the other client by scanning, instead of hard-coding its MAC.
  ESP_NOW_Found found[MAX_FOUND];
  int n = 0;
  Serial.println("Scanning for peers...");
  while (n <= 0) {
    n = ESP_NOW.discover(found, MAX_FOUND, 1000);  // 1 s collection window
    if (n <= 0) {
      Serial.println("  no peers yet, scanning again...");
    }
  }

  // Pick the strongest responder (highest RSSI = closest to 0).
  int best = 0;
  for (int i = 1; i < n; i++) {
    if (found[i].rssi > found[best].rssi) {
      best = i;
    }
  }
  Serial.printf("Found %d device(s); pairing with " MACSTR " (RSSI %d)\n", n, MAC2STR(found[best].mac), found[best].rssi);

  peer = new Peer(found[best].mac);
  if (!peer->add()) {
    Serial.println("Failed to add discovered peer");
    while (true) {
      delay(1000);
    }
  }
  Serial.println("Paired. Starting ping...");
}

void loop() {
  char msg[24];
  int n = snprintf(msg, sizeof(msg), "ping %lu", (unsigned long)seq++);
  peer->send((const uint8_t *)msg, n);

  // Wait 2 s while servicing replies and the send-status URC.
  for (uint32_t t = millis(); millis() - t < 2000;) {
    ESP_NOW.poll();
    delay(5);
  }
}
