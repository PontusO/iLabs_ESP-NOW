/*
    ESP-NOW Unicast Ping  (RP2040/RP2350 host + ESP32-C6 co-processor)

    Minimal unicast demo exercising the "core path": add a known peer, send a
    unicast frame to it once a second, and print delivery status (onSent) and
    any replies (onReceive). Run it on two boards, each pointing PEER_MAC at
    the other's ESP32-C6 STA MAC (read it from ESP_NOW.macAddress()).

    The send is driven by a repeating hardware timer that just sets a flag;
    loop() calls ESP_NOW.poll() every iteration (never blocking) so replies are
    dispatched promptly, and fires the send when the flag is set.
*/

#include <Arduino.h>
#include "ESP32_NOW.h"

#define ESPNOW_WIFI_CHANNEL 6
#define SEND_INTERVAL_MS    1000

// The OTHER board's ESP32-C6 STA MAC. Change this per board.
uint8_t PEER_MAC[6] = {0xF0, 0xF5, 0xBD, 0x31, 0x9B, 0xB0};

// Repeating timer -> "send is due" flag (set in timer/alarm context; the
// actual send runs in loop(), never inside an ISR).
volatile bool send_due = false;
repeating_timer_t send_timer;

bool onSendTimer(repeating_timer_t *rt) {
  (void)rt;
  send_due = true;
  return true;  // keep repeating
}

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

  // The library uses the board variant's ESP32 UART and resets it automatically.
  ESP_NOW.setLink(ESPNOW_WIFI_CHANNEL);

  if (!ESP_NOW.begin() || !peer.add()) {
    Serial.println("ESP-NOW init / peer add failed");
    while (true) {
      delay(1000);
    }
  }

  Serial.println("Unicast ready. My MAC: " + ESP_NOW.macAddress());

  // Fire a send every SEND_INTERVAL_MS via a repeating timer.
  add_repeating_timer_ms(SEND_INTERVAL_MS, onSendTimer, nullptr, &send_timer);
}

void loop() {
  // Service the link every iteration - never blocks, so replies and
  // send-status URCs are dispatched promptly.
  ESP_NOW.poll();

  if (send_due) {
    send_due = false;
    char msg[24];
    int n = snprintf(msg, sizeof(msg), "ping %lu", (unsigned long)seq++);
    peer.send((const uint8_t *)msg, n);
  }
}
