#!/usr/bin/env python3
#
#    Copyright (c) 2026 P. Oldberg <pontus@ilabs.se>
#
#    This library is free software; you can redistribute it and/or
#    modify it under the terms of the GNU Lesser General Public
#    License as published by the Free Software Foundation; either
#    version 2.1 of the License, or (at your option) any later version.
#
#    This library is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
#    Lesser General Public License for more details.
#
"""
iLabs ESP-NOW firmware flasher — front-end for the prebuilt bundles in fw/.

Pick your Challenger board, and this drives the full two-stage flash:

  Stage 1  copy the USB2Serial bridge UF2 onto the RP2040/RP2350 mass-storage
           device (BOOTSEL mode) so the host MCU becomes a USB-to-serial bridge
           for the ESP co-processor.
  Stage 2  flash the ESP-NOW AT firmware (bootloader + partition table + app)
           onto the ESP chip over that serial link, using esptool's Python API
           so we render our own progress bar and monitor the transfer.

The ESP chip has no USB of its own — the RP MCU bridges it — so a FORKED esptool
is required: it adds an `RP2040Reset` strategy that leaves IO0/DTR asserted low,
keeping the ESP in its download bootloader while the RP2040 bridges. Stock pip
esptool will NOT flash these boards. This script refuses to run without the fork.

Usage:
    python3 flash.py                 # interactive menu
    python3 flash.py --list          # list boards, verify esptool fork, exit
    python3 flash.py --board 1       # non-interactive board select (index or dir)
    python3 flash.py --board Challenger_RP2350_WIFI6-BLE5 --port /dev/ttyACM0
    python3 flash.py --dry-run       # show what would happen, no copy/flash

Set ILABS_ESPTOOL_PATH to point at the forked esptool checkout if it is not on
sys.path already. `rich` is used for nicer output if installed (pip install rich).
"""

import argparse
import getpass
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))

# --------------------------------------------------------------------------- #
# Optional pretty output (rich). Everything degrades gracefully without it.
# --------------------------------------------------------------------------- #
try:
    from rich.console import Console

    _con = Console()

    def cprint(msg="", style=None):
        # markup=False so literal brackets like [dry-run] / [===>] aren't parsed.
        _con.print(msg, style=style, highlight=False, markup=False)

    def cinput(prompt):
        return _con.input(prompt)

    HAVE_RICH = True
except Exception:  # rich not installed
    HAVE_RICH = False

    _ANSI = {
        "green": "\033[1;32m",
        "red": "\033[1;31m",
        "yellow": "\033[0;33m",
        "cyan": "\033[1;36m",
        "bold": "\033[1m",
        "dim": "\033[2m",
    }
    _RST = "\033[0m"
    _TTY = hasattr(sys.stdout, "isatty") and sys.stdout.isatty()

    def cprint(msg="", style=None):
        if style and _TTY:
            code = "".join(_ANSI.get(s, "") for s in str(style).split())
            print(f"{code}{msg}{_RST}" if code else msg)
        else:
            print(msg)

    def cinput(prompt):
        return input(prompt)


def die(msg, code=1):
    cprint(f"ERROR: {msg}", style="red")
    sys.exit(code)


# --------------------------------------------------------------------------- #
# Board configuration table. The two RP2040 boards share a mount label and UF2;
# only the user's menu choice tells them apart (different ESP chip + params).
# --------------------------------------------------------------------------- #
BOARDS = [
    {
        "dir": "Challenger_RP2350_WIFI6-BLE5",
        "label": "Challenger RP2350 WiFi6/BLE5  (ESP32-C6)",
        "chip": "esp32c6",
        "mount_label": "RP2350",
        "uf2": "RP2350USB2Serial.ino.uf2",
        "flash_mode": "dio",
        "flash_freq": "80m",
        "flash_size": "4MB",
    },
    {
        "dir": "Challenger_RP2040_WIFI",
        "label": "Challenger RP2040 WiFi        (ESP8285 / esp8266)",
        "chip": "esp8266",
        "mount_label": "RPI-RP2",
        "uf2": "Challenger2040USB2Serial.ino.uf2",
        "flash_mode": "dout",
        "flash_freq": "40m",
        "flash_size": "2MB",
    },
    {
        "dir": "Challenger_RP2040_WIFI-BLE",
        "label": "Challenger RP2040 WiFi/BLE    (ESP32-C3)",
        "chip": "esp32c3",
        "mount_label": "RPI-RP2",
        "uf2": "Challenger2040USB2Serial.ino.uf2",
        "flash_mode": "dio",
        "flash_freq": "40m",
        "flash_size": "keep",
    },
]

# Firmware images written to the ESP flash — common to every board.
FLASH_IMAGES = [
    (0x0000, "bin/bootloader.bin"),
    (0x8000, "bin/partition-table.bin"),
    (0x10000, "bin/esp_now_at.bin"),
]

ROM_BAUD = 115200               # ESP ROM download-loader boot baud
FLASH_BAUD = 921600             # fast baud we try to flash at
BAUD_RETRIES = 3                # attempts at FLASH_BAUD before falling back
CONNECT_MODE = "default-reset"  # forked esptool auto-picks RP2040Reset first
RESET_AFTER = "hard-reset"


# --------------------------------------------------------------------------- #
# esptool discovery + fork verification.
# --------------------------------------------------------------------------- #
def import_esptool(explicit_path=None):
    """Locate and import the FORKED esptool, or exit with guidance."""
    candidates = []
    if explicit_path:
        candidates.append(explicit_path)
    env = os.environ.get("ILABS_ESPTOOL_PATH")
    if env:
        candidates.append(env)
    # Vendored locations (future self-contained bundle), then dev fallback.
    candidates.append(os.path.join(HERE, "vendor", "esptool"))
    candidates.append(os.path.join(HERE, "esptool"))
    candidates.append(os.path.expanduser("~/bin/esptool"))

    # Insert in reverse so the earliest (highest-priority) candidate — an
    # explicit --esptool-path / ILABS_ESPTOOL_PATH — ends up first on sys.path.
    for path in reversed(candidates):
        if path and os.path.isdir(path) and path not in sys.path:
            sys.path.insert(0, path)

    try:
        import esptool  # noqa: E402
        import esptool.reset  # noqa: E402
    except ImportError as e:
        die(
            "could not import esptool.\n"
            "  The iLabs FORK of esptool is required (it carries the RP2040Reset\n"
            "  strategy needed to flash the ESP through the RP2040/RP2350 bridge).\n"
            "  Point ILABS_ESPTOOL_PATH at the fork checkout, e.g.:\n"
            "    export ILABS_ESPTOOL_PATH=~/bin/esptool\n"
            f"  (import error: {e})"
        )

    if not hasattr(esptool.reset, "RP2040Reset"):
        die(
            "the esptool that was imported is NOT the iLabs fork.\n"
            f"  Imported: esptool {getattr(esptool, '__version__', '?')} from\n"
            f"    {os.path.dirname(esptool.__file__)}\n"
            "  It lacks the RP2040Reset strategy and cannot flash these boards.\n"
            "  Set ILABS_ESPTOOL_PATH to the forked esptool checkout."
        )
    return esptool


# --------------------------------------------------------------------------- #
# Serial port helpers (pyserial).
# --------------------------------------------------------------------------- #
def list_serial_ports():
    try:
        from serial.tools import list_ports
    except ImportError:
        die("pyserial is required (pip install pyserial).")
    return {p.device for p in list_ports.comports()}


def looks_like_bridge(dev):
    base = os.path.basename(dev)
    return (
        base.startswith("ttyACM")
        or base.startswith("ttyUSB")
        or base.startswith("cu.usbmodem")
        or base.startswith("cu.usbserial")
    )


def wait_for_new_serial_port(before, timeout=30.0):
    """Poll for a serial port that appeared after `before`, return its device."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        now = list_serial_ports()
        fresh = sorted(d for d in (now - before) if looks_like_bridge(d))
        if fresh:
            return fresh[0]
        time.sleep(0.3)
    return None


# --------------------------------------------------------------------------- #
# RP mass-storage mount detection (cross-platform).
# --------------------------------------------------------------------------- #
def candidate_mount_paths(label):
    user = getpass.getuser()
    if sys.platform == "darwin":
        return [os.path.join("/Volumes", label)]
    # Linux desktop automount locations.
    return [
        os.path.join("/media", user, label),
        os.path.join("/run/media", user, label),
        os.path.join("/media", label),
    ]


def find_mount(label):
    for p in candidate_mount_paths(label):
        if os.path.isdir(p):
            return p
    return None


def wait_for_mount(label, timeout=60.0):
    cprint(f"Waiting for '{label}' mass-storage device (put the board in "
           f"BOOTSEL mode)...", style="cyan")
    deadline = time.time() + timeout
    while time.time() < deadline:
        p = find_mount(label)
        if p:
            cprint(f"  found at {p}", style="dim")
            return p
        time.sleep(0.5)
    return None


# --------------------------------------------------------------------------- #
# Custom esptool logger — routes the flash progress into our own bar.
# --------------------------------------------------------------------------- #
def install_progress_logger(esptool):
    from esptool.logger import log, EsptoolLogger

    tty = hasattr(sys.stdout, "isatty") and sys.stdout.isatty()

    class ProgressLogger(EsptoolLogger):
        _pb_prefix = None

        def progress_bar(self, cur_iter, total_iters, prefix="", suffix="",
                         bar_length=32):
            # New logical bar → finish the previous line first.
            if prefix != self._pb_prefix and self._pb_prefix is not None:
                sys.stdout.write("\n")
            self._pb_prefix = prefix

            frac = (cur_iter / total_iters) if total_iters else 1.0
            frac = max(0.0, min(1.0, frac))
            filled = int(bar_length * frac)
            if filled >= bar_length:
                bar = "=" * bar_length
            elif filled == 0:
                bar = " " * bar_length
            else:
                bar = "=" * (filled - 1) + ">" + " " * (bar_length - filled)
            line = f"  {prefix}[{bar}] {100 * frac:5.1f}% {suffix}"

            if tty:
                sys.stdout.write("\r\033[K" + line)
                if cur_iter >= total_iters:
                    sys.stdout.write("\n")
                    self._pb_prefix = None
                sys.stdout.flush()
            else:
                if cur_iter >= total_iters:
                    print(line)
                    self._pb_prefix = None

    log.set_logger(ProgressLogger())
    return log


# --------------------------------------------------------------------------- #
# The flash procedure.
# --------------------------------------------------------------------------- #
def _close_port(esp):
    try:
        esp._port.close()
    except Exception:
        pass


def _connect_and_prepare(esptool, port, target_baud):
    """Fresh reset -> stub -> (verified) baud change -> attach flash.

    Returns a ready-to-flash esp object. Raises on ANY failure; the caller
    resets and retries. A fresh detect_chip() re-toggles RTS/DTR through the
    fork's RP2040Reset, rebooting the ESP into its ROM download loader at
    115200 — a clean resync no matter what state a botched baud change left.
    """
    esp = esptool.detect_chip(port, connect_mode=CONNECT_MODE)
    try:
        esp = esptool.run_stub(esp)
        if target_baud and target_baud > esp.ESP_ROM_BAUD:
            esp.change_baud(target_baud)
            # The baud switch over the RP2040 bridge is unreliable: the
            # CHANGE_BAUDRATE request/ack can be lost, or the ESP-side switch
            # may silently not take, leaving host and ESP at different rates.
            # Force a round-trip at the NEW baud to prove both ends agree
            # before we commit to flashing.
            esp.read_reg(esp.UART_DATE_REG_ADDR)
        esptool.attach_flash(esp)
        return esp
    except Exception:
        _close_port(esp)
        raise


def _connect_with_recovery(esptool, board, port):
    """Establish a working link, recovering from lost baud-change requests.

    Tries FLASH_BAUD a few times (each attempt hardware-resets and resyncs),
    then falls back to ROM_BAUD with no baud switch at all — slower, but it
    cannot hit the baud-change bug. Returns (esp, baud_used).
    """
    last_err = None
    for attempt in range(1, BAUD_RETRIES + 1):
        try:
            cprint(f"Connecting to {board['chip']} on {port} at {FLASH_BAUD} "
                   f"(attempt {attempt}/{BAUD_RETRIES})...", style="cyan")
            esp = _connect_and_prepare(esptool, port, FLASH_BAUD)
            return esp, FLASH_BAUD
        except esptool.FatalError as e:
            last_err = e
            cprint(f"  link not stable at {FLASH_BAUD}: {e}", style="yellow")
            # Let the OS release the port; the next detect_chip re-resets the ESP.
            time.sleep(0.8)

    cprint(f"Falling back to {ROM_BAUD} baud (no baud switch, slower but "
           f"reliable)...", style="yellow")
    try:
        esp = _connect_and_prepare(esptool, port, None)
        return esp, ROM_BAUD
    except esptool.FatalError as e:
        raise esptool.FatalError(
            f"could not establish a link at any baud. "
            f"Last {ROM_BAUD} error: {e}; last {FLASH_BAUD} error: {last_err}")


def do_flash(esptool, board, port, keep_baud=False):
    board_dir = os.path.join(HERE, board["dir"])
    addr_data = []
    for addr, rel in FLASH_IMAGES:
        path = os.path.join(board_dir, rel)
        if not os.path.isfile(path):
            die(f"missing firmware image: {path}")
        addr_data.append((addr, path))

    if keep_baud:
        cprint(f"\nConnecting to {board['chip']} on {port} at {ROM_BAUD} "
               f"(--keep-baud)...", style="cyan")
        esp = _connect_and_prepare(esptool, port, None)
        baud = ROM_BAUD
    else:
        esp = None
        esp, baud = _connect_with_recovery(esptool, board, port)

    try:
        cprint(f"Flashing ESP-NOW AT firmware at {baud} baud...", style="cyan")
        esptool.write_flash(
            esp,
            addr_data,
            flash_mode=board["flash_mode"],
            flash_freq=board["flash_freq"],
            flash_size=board["flash_size"],
        )
        esptool.reset_chip(esp, RESET_AFTER)
    finally:
        _close_port(esp)


def stage1_copy_bridge(board, dry_run=False):
    board_dir = os.path.join(HERE, board["dir"])
    uf2 = os.path.join(board_dir, board["uf2"])
    if not os.path.isfile(uf2):
        die(f"missing bridge UF2: {uf2}")

    ports_before = list_serial_ports()

    if dry_run:
        cprint(f"[dry-run] would wait for mount '{board['mount_label']}' and copy",
               style="yellow")
        cprint(f"[dry-run]   {uf2}", style="dim")
        return ports_before, None

    mount = wait_for_mount(board["mount_label"])
    if not mount:
        die(f"timed out waiting for '{board['mount_label']}'. Is the board in "
            f"BOOTSEL mode?")

    cprint("Copying USB-to-serial bridge firmware...", style="cyan")
    try:
        shutil.copy(uf2, mount)
        try:
            os.sync()
        except (AttributeError, OSError):
            pass
    except OSError as e:
        # The RP often reboots mid-copy; that is expected once the UF2 is in.
        cprint(f"  (device rebooted during copy: {e})", style="dim")

    return ports_before, mount


# --------------------------------------------------------------------------- #
# Board selection UI.
# --------------------------------------------------------------------------- #
def print_boards():
    cprint("Available boards:", style="bold")
    for i, b in enumerate(BOARDS, 1):
        cprint(f"  {i}. {b['label']}")


def resolve_board(selector):
    """Accept a 1-based index, a directory name, or a case-insensitive match."""
    if selector is None:
        return None
    s = str(selector).strip()
    if s.isdigit():
        idx = int(s)
        if 1 <= idx <= len(BOARDS):
            return BOARDS[idx - 1]
        die(f"board index out of range: {idx}")
    for b in BOARDS:
        if b["dir"].lower() == s.lower():
            return b
    die(f"unknown board: {selector!r} (use --list to see choices)")


def choose_board_interactive():
    print_boards()
    while True:
        try:
            sel = cinput("Select board [1-%d] (q to quit): " % len(BOARDS)).strip()
        except (EOFError, KeyboardInterrupt):
            cprint("\nAborted.", style="yellow")
            sys.exit(130)
        if sel.lower() in ("q", "quit", "exit"):
            sys.exit(0)
        if sel.isdigit() and 1 <= int(sel) <= len(BOARDS):
            return BOARDS[int(sel) - 1]
        cprint("Invalid selection.", style="red")


# --------------------------------------------------------------------------- #
# Main
# --------------------------------------------------------------------------- #
def main():
    global BAUD_RETRIES
    ap = argparse.ArgumentParser(
        description="iLabs ESP-NOW firmware flasher (fw/ bundles).")
    ap.add_argument("--board", help="board index (1..N) or directory name")
    ap.add_argument("--port", help="serial port of the RP bridge "
                    "(default: auto-detect the port that appears after the UF2 copy)")
    ap.add_argument("--esptool-path", help="path to the forked esptool checkout")
    ap.add_argument("--keep-baud", action="store_true",
                    help="flash at the ROM baud (%d) with no baud switch at all" %
                    ROM_BAUD)
    ap.add_argument("--baud-retries", type=int, default=BAUD_RETRIES,
                    help="attempts at %d before falling back to %d "
                    "(default %d)" % (FLASH_BAUD, ROM_BAUD, BAUD_RETRIES))
    ap.add_argument("--list", action="store_true",
                    help="verify the esptool fork, list boards, and exit")
    ap.add_argument("--dry-run", action="store_true",
                    help="show what would happen without copying or flashing")
    args = ap.parse_args()

    if args.baud_retries is not None and args.baud_retries >= 0:
        BAUD_RETRIES = args.baud_retries

    esptool = import_esptool(args.esptool_path)
    ver = getattr(esptool, "__version__", "?")
    cprint(f"esptool fork OK (v{ver}, RP2040Reset present)", style="green")

    if args.list:
        print_boards()
        return 0

    board = resolve_board(args.board) or choose_board_interactive()
    cprint(f"\nSelected: {board['label']}", style="bold")

    if args.dry_run:
        stage1_copy_bridge(board, dry_run=True)
        port = args.port or "<auto-detected ttyACM*/cu.usbmodem*>"
        cprint(f"[dry-run] would flash on {port}:", style="yellow")
        for addr, rel in FLASH_IMAGES:
            cprint(f"[dry-run]   0x{addr:05x}  {board['dir']}/{rel}", style="dim")
        cprint(f"[dry-run]   chip={board['chip']} mode={board['flash_mode']} "
               f"freq={board['flash_freq']} size={board['flash_size']} "
               f"after={RESET_AFTER}", style="dim")
        cprint(f"[dry-run]   baud: try {FLASH_BAUD} x{BAUD_RETRIES} "
               f"(verified round-trip), else fall back to {ROM_BAUD}", style="dim")
        return 0

    # Stage 1: bridge UF2 -> RP mass storage.
    ports_before, _ = stage1_copy_bridge(board)

    # Stage 2a: find the serial bridge port.
    if args.port:
        port = args.port
        cprint(f"Using serial port {port}", style="dim")
    else:
        cprint("Waiting for the serial bridge to enumerate...", style="cyan")
        port = wait_for_new_serial_port(ports_before)
        if not port:
            die("could not detect a new serial port after the UF2 copy. "
                "Re-run with --port <device>.")
        cprint(f"  bridge appeared at {port}", style="dim")
        # Give the bridge a moment to settle before we reset the ESP.
        time.sleep(1.5)

    # Stage 2b: flash the ESP.
    install_progress_logger(esptool)
    try:
        do_flash(esptool, board, port, keep_baud=args.keep_baud)
    except esptool.FatalError as e:
        die(f"ESP flash failed: {e}")
    except KeyboardInterrupt:
        cprint("\nAborted.", style="yellow")
        return 130
    except Exception as e:  # serial errors, etc.
        die(f"unexpected error during flash: {e}")

    cprint("\nESP-NOW firmware flashed OK.", style="green")
    return 0


if __name__ == "__main__":
    sys.exit(main())
