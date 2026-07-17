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
        |  UART  (ESP_SERIAL_PORT, AT+EN)  |
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
   with one line — the library uses the UART the board variant wires to the
   ESP32 (`ESP_SERIAL_PORT`), so the sketch never names a serial port:
   ```cpp
   ESP_NOW.setLink(CH);   // opens the variant's ESP UART + resets the ESP32
   ```
2. **Servicing receives.** Call `ESP_NOW.poll()` regularly from `loop()` (like
   `PubSubClient.loop()` / `ArduinoOTA.handle()`). Received frames are
   dispatched to peer `onReceive()` / `onNewPeer()` from inside `poll()`.
   On RP2040 `delay()` does not yield, so don't sit in a bare `delay()` — the
   examples wrap their waits in a poll loop.

`WiFi.macAddress()` maps to `ESP_NOW.macAddress()` (the co-processor's STA MAC).

## Board reset handling

`setLink()` uses the variant's `ESP_SERIAL_PORT`, and when the variant also
defines the ESP32 reset pins (`PIN_ESP_MODE` / `PIN_ESP_RST` — all the iLabs
Challenger WiFi/WiFi6 boards) it **automatically hardware-resets the ESP32
into run mode and waits for its `+ENREADY`** before returning. So every time
the host boots (power-on or reset), the co-processor gets a clean,
deterministic cold start — no stale peers, keys, or baud left over from a
previous session, and the boot-ROM chatter is discarded for you. (If a
variant defines `ESP_SERIAL_PORT` but not the reset pins, `begin()` still
issues an `AT+ENDEINIT` first to clear prior state as best it can.)

If the ESP32 reboots *unexpectedly* while running (brownout, manual reset),
it emits `+ENREADY`; the library flags it. Register a handler to react:

```cpp
ESP_NOW.onReset([](void *) {
  // The ESP32 lost its peers and keys. Simplest recovery: full clean cycle.
  rp2040.reboot();
}, nullptr);
// ...or poll ESP_NOW.wasReset() from loop().
```

## Hardware / boards

- An **iLabs Challenger WiFi/WiFi6 board** (RP2040 or RP2350) with an on-board
  ESP32-C6/C3 flashed with the iLabs `AT+EN` interpreter. The host↔ESP32 UART
  and the ESP32 reset pins are wired on-board and described by the arduino-pico
  board variant.

The library talks to the ESP32 over the variant's `ESP_SERIAL_PORT` and knows
the reset pins from the variant, so **the sketch never configures a serial
port** — just `ESP_NOW.setLink(<channel>)` (see
[Board reset handling](#board-reset-handling)). It therefore requires a board
variant that defines `ESP_SERIAL_PORT`; on any other board `setLink()` raises
a compile error naming the requirement.

Optionally wire RTS/CTS and enable flow control on the ESP32 (`AT+ENFLOW`) for
high-throughput bursts.

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
  ESP_NOW.setLink(6);   // uses the board variant's ESP32 UART + auto reset
  ESP_NOW.begin();
  peer.add();
}

void loop() {
  peer.send((const uint8_t *)"hello", 5);
  for (uint32_t t = millis(); millis() - t < 1000;) { ESP_NOW.poll(); delay(5); }
}
```

See `examples/` for the ported **Broadcast_Master** / **Broadcast_Slave**
(the arduino-esp32 canonical examples), **Unicast_PingPong**, and
**Discovery_PingPong** (finds the other board with `ESP_NOW.discover()`
instead of a hard-coded MAC).

### Discovery (iLabs extension)

`ESP_NOW.discover()` has no arduino-esp32 equivalent — it drives the iLabs
`AT+ENDISCOVER` firmware feature, where any board running the interpreter
answers a broadcast probe in firmware (no host involvement on the responder):

```cpp
ESP_NOW_Found found[8];
int n = ESP_NOW.discover(found, 8, 1000);   // scan for 1 s on the current channel
for (int i = 0; i < n; i++) {
  // found[i].mac  - responder's STA MAC
  // found[i].rssi - RSSI this board measured
}
```

It blocks for the collection window and returns the count found (or -1 on
error). Only covers the current channel.

## API → AT mapping

| Arduino ESP-NOW call | AT+EN command |
|---|---|
| `ESP_NOW.setLink(ch)` | opens the variant's `ESP_SERIAL_PORT`, hardware-resets the ESP32, sets the channel for `begin()` |
| `ESP_NOW.begin(pmk)` | `ATE0`, `AT+ENINIT=<ch>`, `AT+ENPMK=<hex>` (if pmk), `AT+ENVER?` |
| `ESP_NOW.end()` | remove peers, `AT+ENDEINIT` |
| `peer.add()` | `AT+ENADDPEER=<mac>,<ch>,<enc>[,<lmk>]` |
| `peer.remove()` | `AT+ENDELPEER=<mac>` |
| `peer.send(d,n)` (unicast) | `AT+ENSEND=<mac>,<n>,<hex>` (`AT+ENFRAGSEND` if n>248) |
| `peer.send(d,n)` (broadcast peer) / `ESP_NOW.write(d,n)` | `AT+ENBCAST=<n>,<hex>` |
| `peer.setKey(lmk)` / `setChannel()` | re-issue `AT+ENADDPEER` (in-place modify) |
| `getTotalPeerCount()` / `getEncryptedPeerCount()` | `AT+ENLISTPEER?` |
| `ESP_NOW.macAddress()` | `AT+ENMAC?` |
| `ESP_NOW.discover(out,max,ms)` | `AT+ENDISCOVER[=<ms>]` → `+ENDISCOVER:<mac>,<rssi>` per responder |
| `peer.onReceive(d,n,bcast)` | `+ENRECV:<src>,<n>,<rssi>,<hex>,<dst>` (dispatched in `poll()`) |
| `peer.onSent(ok)` | `+ENSENDOK` / `+ENSENDFAIL` |
| `ESP_NOW.onNewPeer(cb)` | `+ENRECV` from an unregistered source |

The broadcast/unicast distinction in `onReceive()` and `onNewPeer()` comes
from the destination MAC that the ESP-IDF firmware appends to `+ENRECV`.

## Status / limitations (v0.1)

Core path implemented and compile-tested for RP2040 and RP2350: init,
add/remove peer, unicast send + delivery status, broadcast send/receive,
receive dispatch, `onNewPeer`, PMK-encrypted peers, and device discovery
(`ESP_NOW.discover()`).

Not yet wrapped: per-peer PHY rate (the AT firmware exposes rate globally via
`AT+ENRATE`; `setRate()` is a stored no-op), RSSI/stats accessors, and
ESP-NOW v2 (>250 B) framing.
`getMaxDataLen()` reports 250 (ESP-NOW v1); unicast sends above 248 B are
transparently fragmented by the firmware.

Callbacks run in the context of whoever calls `poll()` (or the sending call),
not a background task — keep them short, as with the ESP32 library.
