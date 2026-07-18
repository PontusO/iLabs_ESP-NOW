# iLabs ESP-NOW — regression test rig

A small, permanent two-board bench setup for catching regressions in **this
library** and in the **AT+EN interpreter firmware** as they evolve. Build it
once, keep it wired, and re-run after every change.

The test software is `examples/RegressionSuite` — one sketch, flashed to both
boards. It prints a PASS/FAIL report in two phases:

- **Phase 1 — raw AT protocol.** Drives the AT+EN firmware directly (through
  `ESP_NOW.command()`) and asserts both the OK paths *and* that malformed input
  is rejected with the exact spec'd result (`+ENERR:<n>`, plain `ERROR`, or
  timeout). This is the tripwire for the firmware drifting from the AT+EN
  Command Set Spec v0.1 — bad grammar, wrong command TYPE, out-of-range
  channel, malformed MAC/PMK/LMK, over-length / non-hex payloads.
- **Phase 2 — library API.** Exercises the full public C++ surface (local
  getters, peer management, OTA round-trips) plus negative/boundary cases
  (zero/negative-length sends, add/remove idempotency and count accounting, an
  out-of-range-channel `add()` that must fail, discover() argument guards).

## Bill of materials

| Qty | Item | Notes |
|----|------|-------|
| 2 | iLabs Challenger board with an on-board ESP32-C6 | e.g. **Challenger 2350 WiFi6/BLE** (`rp2040:rp2040:challenger_2350_wifi6_ble5`) or **Challenger 2040 WiFi6/BLE**. The variant must define `ESP_SERIAL_PORT` + `PIN_ESP_MODE`/`PIN_ESP_RST`. |
| 2 | USB-C cables | Power + serial to the host PC |
| 1 | USB hub (optional) | So both boards share one PC |
| — | ~0.3–2 m spacing between the boards | Same room, same bench; avoid touching antennas together (RF overload) |

No jumper wiring is required: the host↔ESP32 UART and the ESP32 reset lines are
on-board, and the library drives them automatically.

## One-time firmware prep (per board)

Each board's **ESP32-C6 must be running the iLabs AT+EN interpreter**
(`iLabs_AT_ESP-now`), not the stock esp-hosted firmware.

1. Flash the AT+EN firmware to the C6 (build it for `esp32c6`, then flash over
   the C6's UART — the Challenger core can put the C6 into download mode via
   `PIN_ESP_MODE` low + reset; use your usual iLabs ESP flashing flow).
2. Record the firmware commit used, so a regression can be bisected against a
   known-good firmware + library pair.

## Software setup

1. Install this library (it already lives in your Arduino `libraries/`).
2. Open `File → Examples → iLabs ESP-NOW → RegressionSuite`.
3. Select the board (e.g. Challenger 2350 WiFi6/BLE) and flash the **same
   sketch to both boards**. No per-board edits.
4. Open a serial monitor at **115200** on each board (or at least on the one
   that prints `role: TESTER`).

Both boards default to **channel 6**; change `ESPNOW_WIFI_CHANNEL` in the
sketch if your bench is noisy on 6 (keep them equal).

## Running

On power-up each board discovers the other and picks a role by MAC (lower MAC
= TESTER). The **TESTER** prints:

```
Regression node. My MAC: 54320406D248
Discovering partner...
Partner F0F5BD319BB0 -> role: TESTER

===== iLabs ESP-NOW regression suite =====

--- Phase 1: raw AT protocol (positive + negative) ---
  [PASS] [AT+] bare AT -> OK
  [PASS] [AT+] ENVER? emits +ENVER:
  ...
  [PASS] [AT-] unknown command -> +ENERR:8
  [PASS] [AT-] SEND len>248 -> +ENERR:4
  [PASS] [AT-] ADDPEER bad-length LMK -> +ENERR:6

--- Phase 2: library API (contract + OTA) ---
  [PASS] macAddress() is 12 hex chars
  [PASS] getVersion() >= 1
  ...
  [PASS] add() with channel 200 -> false (firmware rejects)
  [PASS] encrypted unicast round-trip
  [PASS] peer count dropped after removePeer

===== RESULT: 96 passed, 0 failed =====
>>> ALL TESTS PASSED <<<
```

The **RESPONDER** prints a couple of `RESPONDER: registered tester ...` lines
and otherwise stays quiet (it's the echo/control server).

To re-run, reset the TESTER board (the responder self-heals after ~8 s of
silence, so you don't need to touch it).

## What "PASS" means / interpreting failures

- **All PASS** → library + firmware behave as expected; save this output as the
  baseline for the current commits.
- **A specific test FAILs** → its name points at the broken area (e.g.
  `unicast fragmented round-trip` → fragmentation path; `encrypted unicast
  round-trip` → PMK/LMK or `AT+ENADDPEER` encryption). Compare against the
  baseline to see what changed.
- **Everything after warm-up FAILs** → the two boards aren't talking: check both
  are on the same channel, both C6s run the AT+EN firmware, and they're powered.
- **Nothing prints / stuck at "Discovering partner..."** → only one board is up,
  or a C6 isn't running the AT+EN firmware (discovery needs both to answer).

## Coverage notes / known gaps

- The fragmented round-trip uses a 250-byte payload (2 fragments). To exercise
  larger reassembly, build with `-DILABS_ESPNOW_LINE_MAX=2048` (bigger host RX
  line buffer) and raise the `big[]` size in the sketch.
- PHY-rate (`setRate`) is validated only as stored state (the AT firmware
  exposes rate globally via `AT+ENRATE`, not per peer).
- RSSI/stats accessors are not yet in the library, so not covered here.

## Suggested workflow

Run the suite after **any** change to `iLabs_ESP-NOW` or `iLabs_AT_ESP-now`,
before committing. Keep the last known-good report checked in or noted next to
the commit hashes so regressions are a quick diff away.
