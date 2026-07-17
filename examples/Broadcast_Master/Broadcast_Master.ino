/*
    ESP-NOW Broadcast Master  (RP2040/RP2350 host + ESP32-C6 co-processor)

    Port of the arduino-esp32 ESP_NOW "Broadcast Master" example to the iLabs
    host-bridge library. The ESP-NOW/ESP_NOW_Peer code is unchanged; only the
    radio bring-up differs: instead of the ESP32 WiFi calls, bind the AT link
    to the ESP32-C6 with ESP_NOW.setLink(), and pump it with ESP_NOW.poll().

    Broadcasts "Hello, World! #n" every 5 seconds to all listeners.
    Pair with the Broadcast_Slave example on another board.
*/

#include <Arduino.h>
#include "ESP32_NOW.h"

/* Definitions */

#define ESPNOW_WIFI_CHANNEL 6

/* Classes */

// Creating a new class that inherits from the ESP_NOW_Peer class is required.
class ESP_NOW_Broadcast_Peer : public ESP_NOW_Peer {
public:
  // Constructor of the class using the broadcast address
  ESP_NOW_Broadcast_Peer(uint8_t channel, wifi_interface_t iface, const uint8_t *lmk) : ESP_NOW_Peer(ESP_NOW.BROADCAST_ADDR, channel, iface, lmk) {}

  ~ESP_NOW_Broadcast_Peer() {
    remove();
  }

  // Initialize ESP-NOW and register the broadcast peer
  bool begin() {
    if (!ESP_NOW.begin() || !add()) {
      Serial.println("Failed to initialize ESP-NOW or register the broadcast peer");
      return false;
    }
    return true;
  }

  // Send a message to all devices within the network
  bool send_message(const uint8_t *data, size_t len) {
    if (!send(data, len)) {
      Serial.println("Failed to broadcast message");
      return false;
    }
    return true;
  }
};

/* Global Variables */

uint32_t msg_count = 0;

// Create a broadcast peer object
ESP_NOW_Broadcast_Peer broadcast_peer(ESPNOW_WIFI_CHANNEL, WIFI_IF_STA, nullptr);

/* Main */

void setup() {
  Serial.begin(115200);

  // Bring up the link to the ESP32 co-processor (replaces the ESP32 WiFi
  // bring-up). The library uses the board variant's ESP32 UART and resets the
  // co-processor automatically - no serial port to configure here.
  ESP_NOW.setLink(ESPNOW_WIFI_CHANNEL);

  Serial.println("ESP-NOW Example - Broadcast Master");
  Serial.println("  MAC Address: " + ESP_NOW.macAddress());
  Serial.printf("  Channel: %u\n", ESPNOW_WIFI_CHANNEL);

  // Register the broadcast peer
  if (!broadcast_peer.begin()) {
    Serial.println("Failed to initialize broadcast peer");
    Serial.println("Rebooting in 5 seconds...");
    delay(5000);
    rp2040.reboot();
  }

  Serial.printf("ESP-NOW version: %d, max data length: %d\n", ESP_NOW.getVersion(), ESP_NOW.getMaxDataLen());
  Serial.println("Setup complete. Broadcasting messages every 5 seconds.");
}

void loop() {
  // Broadcast a message to all devices within the network
  char data[32];
  snprintf(data, sizeof(data), "Hello, World! #%lu", (unsigned long)msg_count++);

  Serial.printf("Broadcasting message: %s\n", data);

  if (!broadcast_peer.send_message((uint8_t *)data, sizeof(data))) {
    Serial.println("Failed to broadcast message");
  }

  // Wait 5 s, servicing the link so send-status and inbound frames dispatch.
  for (uint32_t t = millis(); millis() - t < 5000;) {
    ESP_NOW.poll();
    delay(10);
  }
}
