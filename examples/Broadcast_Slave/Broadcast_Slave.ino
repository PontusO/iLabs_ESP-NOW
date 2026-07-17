/*
    ESP-NOW Broadcast Slave  (RP2040/RP2350 host + ESP32-C6 co-processor)

    Port of the arduino-esp32 ESP_NOW "Broadcast Slave" example to the iLabs
    host-bridge library. Receives broadcast messages; an unknown sender is
    registered as a new master via the onNewPeer() callback, after which its
    frames arrive through that peer's onReceive().

    Pair with the Broadcast_Master example on another board.
*/

#include <Arduino.h>
#include "ESP32_NOW.h"

#include <vector>

/* Definitions */

#define ESPNOW_WIFI_CHANNEL 6
#define ESPNOW_LINK         Serial1     // UART to the ESP32-C6 AT interpreter
#define ESPNOW_LINK_BAUD    115200

/* Classes */

// Creating a new class that inherits from the ESP_NOW_Peer class is required.
class ESP_NOW_Peer_Class : public ESP_NOW_Peer {
public:
  ESP_NOW_Peer_Class(const uint8_t *mac_addr, uint8_t channel, wifi_interface_t iface, const uint8_t *lmk) : ESP_NOW_Peer(mac_addr, channel, iface, lmk) {}

  ~ESP_NOW_Peer_Class() {}

  // Register the master peer
  bool add_peer() {
    if (!add()) {
      Serial.println("Failed to register the broadcast peer");
      return false;
    }
    return true;
  }

  // Print received messages from the master
  void onReceive(const uint8_t *data, size_t len, bool broadcast) {
    Serial.printf("Received a message from master " MACSTR " (%s)\n", MAC2STR(addr()), broadcast ? "broadcast" : "unicast");
    Serial.printf("  Message: %s\n", (char *)data);
  }
};

/* Global Variables */

// Populated as new masters are registered. Pointers avoid dangling refs on
// vector reallocation.
std::vector<ESP_NOW_Peer_Class *> masters;

/* Callbacks */

// Called when an unknown peer sends a message
void register_new_master(const esp_now_recv_info_t *info, const uint8_t *data, int len, void *arg) {
  (void)data;
  (void)len;
  (void)arg;
  if (memcmp(info->des_addr, ESP_NOW.BROADCAST_ADDR, 6) == 0) {
    Serial.printf("Unknown peer " MACSTR " sent a broadcast message\n", MAC2STR(info->src_addr));
    Serial.println("Registering the peer as a master");

    ESP_NOW_Peer_Class *new_master = new ESP_NOW_Peer_Class(info->src_addr, ESPNOW_WIFI_CHANNEL, WIFI_IF_STA, nullptr);
    if (!new_master->add_peer()) {
      Serial.println("Failed to register the new master");
      delete new_master;
      return;
    }
    masters.push_back(new_master);
    Serial.printf("Registered master " MACSTR " (total masters: %u)\n", MAC2STR(new_master->addr()), (unsigned)masters.size());
  } else {
    log_v("Ignoring a unicast message from an unknown peer");
  }
}

/* Main */

void setup() {
  Serial.begin(115200);

  // Bring up the link to the ESP32-C6 co-processor.
  ESPNOW_LINK.begin(ESPNOW_LINK_BAUD);
  ESP_NOW.setLink(ESPNOW_LINK, ESPNOW_WIFI_CHANNEL);

  Serial.println("ESP-NOW Example - Broadcast Slave");
  Serial.println("  MAC Address: " + ESP_NOW.macAddress());
  Serial.printf("  Channel: %u\n", ESPNOW_WIFI_CHANNEL);

  if (!ESP_NOW.begin()) {
    Serial.println("Failed to initialize ESP-NOW");
    Serial.println("Rebooting in 5 seconds...");
    delay(5000);
    rp2040.reboot();
  }

  Serial.printf("ESP-NOW version: %d, max data length: %d\n", ESP_NOW.getVersion(), ESP_NOW.getMaxDataLen());

  // Register the new-peer callback
  ESP_NOW.onNewPeer(register_new_master, nullptr);

  Serial.println("Setup complete. Waiting for a master to broadcast a message...");
}

void loop() {
  // Service the link: inbound frames are dispatched to onReceive()/onNewPeer().
  ESP_NOW.poll();
  delay(1);
}
