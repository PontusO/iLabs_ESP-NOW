# iLabs ESP-NOW firmware bundles + flasher

This directory holds the prebuilt **AT+EN interpreter firmware** for the ESP
co-processor on each Challenger board, plus `flash.py`, a front-end that flashes
the board you pick.

## Boards

| # | Board | ESP chip |
|---|-------|----------|
| 1 | Challenger RP2350 WiFi6/BLE5 | ESP32-C6 |
| 2 | Challenger RP2040 WiFi | ESP8285 (esp8266) |
| 3 | Challenger RP2040 WiFi/BLE | ESP32-C3 |

Each `Challenger_*/` subdirectory contains the USB-to-serial bridge UF2 for the
RP2040/RP2350 host and a `bin/` set (`bootloader.bin`, `partition-table.bin`,
`esp_now_at.bin`) for the ESP chip.

## How flashing works

The ESP co-processor has no USB of its own — the RP2040/RP2350 bridges it. So
`flash.py` runs two stages:

1. **Bridge:** copies the board's USB2Serial UF2 onto the RP mass-storage device
   (the board must be in **BOOTSEL** mode). The RP reboots as a USB-to-serial
   bridge.
2. **Flash:** writes the ESP-NOW AT firmware to the ESP chip over that serial
   link, via esptool's Python API, showing a progress bar.

## Requirements

- **The iLabs fork of esptool (v5.x).** It is mandatory: it adds the
  `RP2040Reset` strategy that holds IO0/DTR low so the ESP stays in its download
  bootloader while the RP2040 bridges. Stock pip esptool **cannot** flash these
  boards, and `flash.py` refuses to run without the fork. Point it at your
  checkout if it is not already importable:

  ```sh
  export ILABS_ESPTOOL_PATH=~/bin/esptool
  ```

  Discovery order: `--esptool-path` → `$ILABS_ESPTOOL_PATH` → `fw/vendor/esptool`
  → `fw/esptool` → `~/bin/esptool`.
- **pyserial** (`pip install pyserial`).
- **rich** is optional (`pip install rich`) for nicer output; without it the
  script falls back to plain/ANSI output.

## Usage

```sh
python3 flash.py                 # interactive menu
python3 flash.py --list          # verify the esptool fork, list boards, exit
python3 flash.py --board 1       # non-interactive (index or directory name)
python3 flash.py --board Challenger_RP2350_WIFI6-BLE5 --port /dev/ttyACM0
python3 flash.py --dry-run       # show what would happen, no copy/flash
```

The serial port is auto-detected (the port that newly appears after the UF2
copy); use `--port` to force one.

### Baud-rate recovery

Flashing runs at 921600 baud for speed, which means switching up from the ESP's
115200 ROM boot baud. Over the RP2040/RP2350 serial bridge that switch is
sometimes unreliable — the `CHANGE_BAUDRATE` request or its ack gets lost, or the
ESP-side switch silently doesn't take, leaving the host and ESP at different rates
so everything afterward fails.

`flash.py` handles this automatically:

1. After each baud switch it forces a register round-trip at the new baud to
   confirm both ends agree. If that check fails, the switch didn't take.
2. On failure it hardware-resets the ESP (a fresh reset re-enters the ROM loader
   at 115200 — a clean resync) and retries, up to `--baud-retries` times
   (default 3).
3. If all fast attempts fail, it falls back to flashing at 115200 with **no** baud
   switch at all — slower, but it cannot hit the bug.

Use `--keep-baud` to skip the fast path entirely and flash at 115200 from the
start, or `--baud-retries N` to tune the number of 921600 attempts.

Supported hosts: Linux (`/media`, `/run/media`) and macOS (`/Volumes`).

The older per-directory `flashit` bash scripts still work and are kept for
reference; `flash.py` supersedes them with a unified, cross-platform UX.
