# iLabs ESP-NOW (Arduino, RP2040/RP2350 host)

The **official Arduino ESP-NOW API** — the arduino-esp32 `ESP_NOW` class
(global `ESP_NOW` instance + `ESP_NOW_Peer` subclassing) — running on an
**RP2040 / RP2350** host, with the radio provided by an **ESP32-C6/C3
co-processor** flashed with the [iLabs AT+EN ESP-NOW interpreter](../iLabs_AT_ESP-now).
The library speaks that AT protocol over a UART link, so sketches written for
the ESP32 `ESP_NOW` library build against this one with only the radio
bring-up changed.

```
   RP2040 / RP2350 host              ESP32-C6 slave
  (this Arduino library)          (AT+EN interpreter)
        |                                  |
        |  UART  (Serial1, AT+EN)          |
        | ESP_NOW.begin() --> AT+ENINIT -->|            ESP-NOW
        | peer.send()   --> AT+ENSEND  --->| ))) 2.4GHz ((( peers
        | ESP_NOW.poll()<-- +ENRECV -------|
```

## What's the same, what's different

The `ESP_NOW` / `ESP_NOW_Peer` surface is source-compatible with
arduino-esp32, so peer classes, `begin()`, `add()`, `send()`, `onReceive()`,
`onSent()`, `onNewPeer()`, broadcast peers and `ESP_NOW.write()` all work as
written. Because the radio is a co-processor over UART rather than on-chip,
**two lines change** versus an ESP32 sketch:

1. **Link bring-up.** Replace the ESP32 Wi-Fi bring-up
   ```cpp
   WiFi.mode(WIFI_STA);
   WiFi.setChannel(CH);
   while (!WiFi.STA.started()) delay(100);
   ```
   with binding the AT link and channel:
   ```cpp
   Serial1.begin(115200);
   ESP_NOW.setLink(Serial1, CH);
   ```
2. **Servicing receives.** Call `ESP_NOW.poll()` regularly from `loop()` (like
   `PubSubClient.loop()` / `ArduinoOTA.handle()`). Received frames are
   dispatched to peer `onReceive()` / `onNewPeer()` from inside `poll()`.
   On RP2040 `delay()` does not yield, so don't sit in a bare `delay()` — the
   examples wrap their waits in a poll loop.

`WiFi.macAddress()` maps to `ESP_NOW.macAddress()` (the co-processor's STA MAC).

## Hardware / wiring

- A host board (RP2040 or RP2350) running the arduino-pico core.
- An ESP32-C6 (or C3) flashed with the iLabs `AT+EN` interpreter.
- A UART between them: host `Serial1` TX/RX ↔ the ESP32's AT-link UART pins,
  plus GND. 115200 8N1 by default. Optionally wire RTS/CTS and enable flow
  control on the ESP32 (`AT+ENFLOW`) for high-throughput bursts.

Pick the host UART with `ESP_NOW.setLink(<Serial>, <channel>)`. Any
`HardwareSerial` works; the examples use `Serial1`.

## Install

Copy or symlink this folder into your Arduino `libraries/` directory, or add
it as an arduino-cli library. It targets the `rp2040` architecture
(arduino-pico core).

## Quick start (unicast)

```cpp
#include <Arduino.h>
#include "ESP32_NOW.h"

class Peer : public ESP_NOW_Peer {
public:
  Peer(const uint8_t *mac) : ESP_NOW_Peer(mac, 6, WIFI_IF_STA, nullptr) {}
  void onReceive(const uint8_t *data, size_t len, bool bcast) {
    Serial.write(data, len); Serial.println();
  }
  using ESP_NOW_Peer::add;
  using ESP_NOW_Peer::send;
};

uint8_t peerMac[6] = {0xF0,0xF5,0xBD,0x31,0x9B,0xB0};
Peer peer(peerMac);

void setup() {
  Serial.begin(115200);
  Serial1.begin(115200);
  ESP_NOW.setLink(Serial1, 6);
  ESP_NOW.begin();
  peer.add();
}

void loop() {
  peer.send((const uint8_t *)"hello", 5);
  for (uint32_t t = millis(); millis() - t < 1000;) { ESP_NOW.poll(); delay(5); }
}
```

See `examples/` for the ported **Broadcast_Master** / **Broadcast_Slave**
(the arduino-esp32 canonical examples) and **Unicast_PingPong**.

## API → AT mapping

| Arduino ESP-NOW call | AT+EN command |
|---|---|
| `ESP_NOW.setLink(Serial1, ch)` | (binds the UART; sets the channel for `begin()`) |
| `ESP_NOW.begin(pmk)` | `ATE0`, `AT+ENINIT=<ch>`, `AT+ENPMK=<hex>` (if pmk), `AT+ENVER?` |
| `ESP_NOW.end()` | remove peers, `AT+ENDEINIT` |
| `peer.add()` | `AT+ENADDPEER=<mac>,<ch>,<enc>[,<lmk>]` |
| `peer.remove()` | `AT+ENDELPEER=<mac>` |
| `peer.send(d,n)` (unicast) | `AT+ENSEND=<mac>,<n>,<hex>` (`AT+ENFRAGSEND` if n>248) |
| `peer.send(d,n)` (broadcast peer) / `ESP_NOW.write(d,n)` | `AT+ENBCAST=<n>,<hex>` |
| `peer.setKey(lmk)` / `setChannel()` | re-issue `AT+ENADDPEER` (in-place modify) |
| `getTotalPeerCount()` / `getEncryptedPeerCount()` | `AT+ENLISTPEER?` |
| `ESP_NOW.macAddress()` | `AT+ENMAC?` |
| `peer.onReceive(d,n,bcast)` | `+ENRECV:<src>,<n>,<rssi>,<hex>,<dst>` (dispatched in `poll()`) |
| `peer.onSent(ok)` | `+ENSENDOK` / `+ENSENDFAIL` |
| `ESP_NOW.onNewPeer(cb)` | `+ENRECV` from an unregistered source |

The broadcast/unicast distinction in `onReceive()` and `onNewPeer()` comes
from the destination MAC that the ESP-IDF firmware appends to `+ENRECV`.

## Status / limitations (v0.1)

Core path implemented and compile-tested for RP2040 and RP2350: init,
add/remove peer, unicast send + delivery status, broadcast send/receive,
receive dispatch, `onNewPeer`, PMK-encrypted peers.

Not yet wrapped: per-peer PHY rate (the AT firmware exposes rate globally via
`AT+ENRATE`; `setRate()` is a stored no-op), device discovery
(`AT+ENDISCOVER`), RSSI/stats accessors, and ESP-NOW v2 (>250 B) framing.
`getMaxDataLen()` reports 250 (ESP-NOW v1); unicast sends above 248 B are
transparently fragmented by the firmware.

Callbacks run in the context of whoever calls `poll()` (or the sending call),
not a background task — keep them short, as with the ESP32 library.
