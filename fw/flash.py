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
    python3 flash.py --skip-bridge   # bridge already running: flash the ESP only
    python3 flash.py --dry-run       # show what would happen, no copy/flash

Set ILABS_ESPTOOL_PATH to point at the forked esptool checkout if it is not on
sys.path already. `rich` is used for nicer output if installed (pip install rich).
"""

import argparse
import getpass
import os
import re
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
        "dir": "Challenger_RP2040_WIFI",
        "label": "Challenger RP2040 WiFi        (ESP8285 / esp8266)",
        "chip": "esp8266",
        "mount_label": "RPI-RP2",
        "uf2": "RP2040USB2Serial.ino.uf2",
        "flash_mode": "dout",
        "flash_freq": "40m",
        "flash_size": "2MB",
    },
    {
        "dir": "Challenger_RP2040_WIFI-BLE",
        "label": "Challenger RP2040 WiFi/BLE    (ESP32-C3)",
        "chip": "esp32c3",
        "mount_label": "RPI-RP2",
        "uf2": "RP2040USB2Serial.ino.uf2",
        "flash_mode": "dio",
        "flash_freq": "40m",
        "flash_size": "keep",
    },
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
PORT_SETTLE_S = 2.0             # settle after the bridge port appears, before
                                # flashing — the freshly-booted CDC needs a
                                # moment or the 921600 baud switch fails and
                                # forces the slow 115200 fallback (the old
                                # flashit scripts slept ~3s here for this).
BRIDGE_RETRIES = 3              # whole-flash retries if the bridge watchdog-resets
                                # (hangs, reboots, re-enumerates) mid-operation.
BAUD_SETTLE_S = 0.5             # quiet pause after change_baud(). The RP2040/
                                # RP2350 bridge services the host's SET_LINE_CODING
                                # (the new baud) from its main loop, so it needs a
                                # moment with NO command traffic to apply it. An
                                # immediate probe both robs it of that moment and
                                # fails without esptool's command-level retries,
                                # which forced the spurious 115200 fallback.


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


def wait_for_port_present(port, timeout=30.0):
    """Wait until the serial device node actually exists.

    The old flashit scripts did exactly this (devwait.py) before handing the
    port to esptool. With an explicit --port we otherwise try to use the device
    the instant the bridge UF2 is copied — before the RP has rebooted and
    re-enumerated as a serial bridge — so the connect fails outright.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(port):
            return True
        time.sleep(0.3)
    return False


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
# Custom esptool logger — a single in-place "frame": one compound, per-partition
# coloured progress bar plus a couple of live stat lines underneath it.
#
# The overall percentage is TRUE byte-based progress across every partition. The
# coloured segments are byte-weighted so the app (which is ~97% of the bytes)
# dominates the bar, but each partition keeps a small minimum width so a tiny one
# (bootloader / partition-table) still shows its colour. While a flash is active
# esptool's own chatter (erase / "Wrote..." / hash lines) is silenced by flipping
# verbosity to "silent" so the frame stays a stable block; real errors still get
# through (error() forces itself past the silent gate onto stderr).
#
# NOTE on installation: EsptoolLogger is a __new__-based singleton, so calling a
# subclass constructor just hands back the same instance and log.set_logger() is
# a no-op for it. The only thing that actually swaps in our methods is rebinding
# the singleton's class directly (log.__class__ = ProgressLogger), which keeps
# every base method (print/stage/set_verbosity/...) intact.
# --------------------------------------------------------------------------- #
_PART_PALETTE = (45, 213, 82, 214, 141, 208, 39, 220)  # 256-colour fg codes
_BAR_CELLS = 46          # width of the compound bar, in cells
_MIN_SEG = 3             # min coloured cells per partition so its hue is visible
_ADDR_RE = re.compile(r"0x([0-9a-fA-F]+)")


def _human_bytes(n):
    n = float(n)
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024.0 or unit == "GB":
            return f"{int(n)} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024.0


def install_progress_logger(esptool):
    from esptool.logger import log, EsptoolLogger

    tty = hasattr(sys.stdout, "isatty") and sys.stdout.isatty()

    class ProgressLogger(EsptoolLogger):
        # set_logger() cannot swap this singleton's class (see note above), and
        # __init__ never runs on the rebind, so ALL state is class-level here and
        # (re)initialised in begin_plan(). Never rely on __init__.
        _plan = None
        _prev_verbosity = None
        _frame_open = False
        _last_draw = 0.0
        _idx = 0
        _local = 0.0
        _pb_prefix = None        # used only by the plain fallback bar

        # ---- plan lifecycle ------------------------------------------------ #
        def begin_plan(self, partitions):
            """partitions: list of {"name", "base", "size", "color"} in flash
            order. Brackets a write_flash() so progress_bar() can render the
            compound frame and esptool's own output stays out of the way."""
            cum = 0
            for p in partitions:
                p["cum_before"] = cum
                cum += p["size"]
            self._plan = partitions
            self._total = cum or 1
            self._t0 = time.time()
            self._frame_open = False
            self._last_draw = 0.0
            self._idx = 0
            self._local = 0.0
            self._prev_verbosity = self._verbosity
            self.set_verbosity("silent")

        def end_plan(self):
            if self._plan is None:
                return
            if tty and self._frame_open:
                self._render(force=True, finish=True)
                sys.stdout.write("\n")
                sys.stdout.flush()
            elif not tty:
                print(f"  flash complete: {_human_bytes(self._total)} written")
            if self._prev_verbosity is not None:
                self.set_verbosity(self._prev_verbosity)
            self._plan = None
            self._frame_open = False

        # ---- the hook esptool calls ---------------------------------------- #
        def progress_bar(self, cur_iter, total_iters, prefix="", suffix="",
                         bar_length=32):
            if self._plan is None:
                return self._simple_bar(cur_iter, total_iters, prefix, suffix,
                                        bar_length)
            # Which partition are we in? Key off the "Writing at 0x...." address:
            # it is base + bytes_written, so the last partition whose base it has
            # reached is the current one (partitions flash in ascending order).
            m = _ADDR_RE.search(prefix)
            addr = int(m.group(1), 16) if m else 0
            idx = 0
            for i, p in enumerate(self._plan):
                if addr >= p["base"]:
                    idx = i
            crossed = idx != self._idx        # partition boundary: always paint
            self._idx = idx
            self._local = (max(0.0, min(1.0, cur_iter / total_iters))
                           if total_iters else 1.0)
            # Small partitions can flash faster than the redraw throttle; force a
            # frame at each boundary so every partition's colour is seen.
            self._render(force=crossed)

        # ---- rendering ----------------------------------------------------- #
        def _render(self, force=False, finish=False):
            now = time.time()
            local = 1.0 if finish else self._local
            last = self._idx == len(self._plan) - 1
            complete = finish or (last and local >= 1.0)
            # Throttle to ~20 fps; always draw the very first and last frames.
            if (not force and not complete and self._frame_open
                    and (now - self._last_draw) < 0.05):
                return
            self._last_draw = now

            lines = self._frame_lines(now, finish)
            if not tty:
                # Dumb terminal: emit one line only at each partition boundary.
                if finish or local >= 1.0:
                    p = self._plan[self._idx]
                    gfrac = (p["cum_before"] + local * p["size"]) / self._total
                    print(f"  {p['name']}: 100%   overall {100 * gfrac:5.1f}%")
                return
            self._paint(lines)

        def _frame_lines(self, now, finish):
            """Build the fixed block of display lines (with ANSI). Pure: no I/O,
            so it can be unit-tested by stripping the escape codes."""
            plan = self._plan
            idx = self._idx
            local = 1.0 if finish else self._local
            cur = plan[idx]
            done = self._total if finish else (cur["cum_before"] + local * cur["size"])
            gfrac = max(0.0, min(1.0, done / self._total))
            elapsed = max(1e-6, now - self._t0)
            rate = done / elapsed
            cur_bytes = cur["size"] if finish else int(local * cur["size"])

            seg_w = self._segment_widths()
            bar = self._compound_bar(seg_w, idx, local, finish)
            rate_s = f"{_human_bytes(rate)}/s" if elapsed > 0.4 else "..."

            return [
                f"  Flashing ESP-NOW AT firmware  •  {len(plan)} files  •  "
                f"{_human_bytes(self._total)} total",
                f"  [{bar}]  {100 * gfrac:5.1f}%",
                "   " + self._legend(idx, finish),
                f"  ● {cur['name']}  0x{cur['base']:08x}  "
                f"{_human_bytes(cur_bytes)} / {_human_bytes(cur['size'])}",
                f"  elapsed {elapsed:4.1f}s  •  {rate_s}  •  "
                f"file {idx + 1}/{len(plan)}",
            ]

        def _paint(self, lines):
            out = sys.stdout
            if not self._frame_open:
                out.write("\n".join(lines) + "\n")
                self._frame_open = True
            else:
                out.write(f"\033[{len(lines)}A")          # cursor up N lines
                for ln in lines:
                    out.write("\r\033[2K" + ln + "\n")     # clear + rewrite
            out.flush()

        def _segment_widths(self):
            """Cells per partition: byte-weighted, but at least _MIN_SEG each so
            small partitions stay visible. Always sums to _BAR_CELLS."""
            plan = self._plan
            n = len(plan)
            cells = _BAR_CELLS
            if cells <= _MIN_SEG * n:
                w = [cells // n] * n
                w[-1] += cells - sum(w)
                return w
            rem = cells - _MIN_SEG * n
            w = [_MIN_SEG + int(round(rem * p["size"] / self._total)) for p in plan]
            # Absorb rounding drift into the biggest (app) segment.
            biggest = max(range(n), key=lambda i: plan[i]["size"])
            w[biggest] += cells - sum(w)
            return w

        def _compound_bar(self, seg_w, idx, local, finish):
            parts = []
            for i, w in enumerate(seg_w):
                c = self._plan[i]["color"]
                if finish or i < idx:
                    filled = w
                elif i > idx:
                    filled = 0
                else:
                    filled = max(0, min(w, int(round(w * local))))
                if filled:
                    parts.append(f"\033[1;38;5;{c}m" + "█" * filled)
                if w - filled:
                    parts.append(f"\033[2;38;5;{c}m" + "░" * (w - filled))
            parts.append("\033[0m")
            return "".join(parts)

        def _legend(self, idx, finish):
            chips = []
            for i, p in enumerate(self._plan):
                c = p["color"]
                mark = "✓" if finish or i < idx else "►" if i == idx else "·"
                chips.append(f"\033[38;5;{c}m●\033[0m {p['name']} {mark}")
            return "   ".join(chips)

        # ---- plain fallback bar (stray progress calls outside a plan) ------- #
        def _simple_bar(self, cur_iter, total_iters, prefix, suffix, bar_length):
            if prefix != self._pb_prefix and self._pb_prefix is not None:
                sys.stdout.write("\n")
            self._pb_prefix = prefix
            frac = max(0.0, min(1.0, (cur_iter / total_iters) if total_iters else 1.0))
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
            elif cur_iter >= total_iters:
                print(line)
                self._pb_prefix = None

    # Rebind the singleton's class directly — the only install that actually
    # takes effect (set_logger() is a no-op for this singleton).
    log.__class__ = ProgressLogger
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
            # change_baud() switches the ESP stub and the host port; the latter
            # sends the bridge a SET_LINE_CODING for the new baud, which the bridge
            # applies from its main loop. Give it a quiet moment to do so before we
            # talk to the ESP again, then let the normal flash flow (attach_flash /
            # write_flash, whose commands esptool retries) be the real check. Do
            # NOT probe here: an un-retried read at the new baud raced the bridge's
            # switch and forced the slow 115200 fallback. A switch that truly never
            # takes makes attach_flash below fail, so the caller still falls back.
            time.sleep(BAUD_SETTLE_S)
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
    from esptool.logger import log
    board_dir = os.path.join(HERE, board["dir"])
    addr_data = []
    plan = []
    for i, (addr, rel) in enumerate(FLASH_IMAGES):
        path = os.path.join(board_dir, rel)
        if not os.path.isfile(path):
            die(f"missing firmware image: {path}")
        addr_data.append((addr, path))
        plan.append({
            "name": os.path.splitext(os.path.basename(rel))[0],
            "base": addr,
            "size": os.path.getsize(path),
            "color": _PART_PALETTE[i % len(_PART_PALETTE)],
        })

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
        # Bracket write_flash so our compound frame owns the terminal for the
        # duration; end_plan() restores esptool's normal output for reset_chip.
        if hasattr(log, "begin_plan"):
            log.begin_plan(plan)
        try:
            esptool.write_flash(
                esp,
                addr_data,
                flash_mode=board["flash_mode"],
                flash_freq=board["flash_freq"],
                flash_size=board["flash_size"],
            )
        finally:
            if hasattr(log, "end_plan"):
                log.end_plan()
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

    time.sleep(0.5)

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


def find_existing_bridge_port():
    """Pick an already-present serial bridge port (for --skip-bridge).

    With the bridge already running no new port appears, so auto-detection can't
    diff before/after. Take the sole bridge-like port if there is exactly one;
    otherwise make the user disambiguate with --port.
    """
    existing = sorted(d for d in list_serial_ports() if looks_like_bridge(d))
    if not existing:
        die("no serial bridge port found. Is the bridge firmware running? "
            "Pass --port <device>.")
    if len(existing) > 1:
        die(f"multiple candidate bridge ports {existing}; pick one with --port.")
    return existing[0]


def wait_for_bridge_back(explicit_port, prev_port, timeout=30.0):
    """Wait for the bridge port to come back after a watchdog reset.

    The watchdog reboot re-enumerates USB, so the port briefly disappears and
    returns, sometimes with a different name. Prefer the explicit/previous name
    if it reappears; otherwise take the sole bridge-like port. Returns the device
    or None on timeout.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        ports = sorted(d for d in list_serial_ports() if looks_like_bridge(d))
        for pref in (explicit_port, prev_port):
            if pref and pref in ports:
                return pref
        if len(ports) == 1:
            return ports[0]
        time.sleep(0.5)
    return None


# --------------------------------------------------------------------------- #
# Main
# --------------------------------------------------------------------------- #
def main():
    global BAUD_RETRIES, PORT_SETTLE_S
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
    ap.add_argument("--settle", type=float, default=PORT_SETTLE_S,
                    help="seconds to wait after the bridge port appears before "
                    "flashing (default %.1f)" % PORT_SETTLE_S)
    ap.add_argument("--skip-bridge", action="store_true",
                    help="skip stage 1 (the bridge UF2 copy / BOOTSEL step) and "
                    "flash the ESP over an already-running bridge")
    ap.add_argument("--list", action="store_true",
                    help="verify the esptool fork, list boards, and exit")
    ap.add_argument("--dry-run", action="store_true",
                    help="show what would happen without copying or flashing")
    args = ap.parse_args()

    if args.baud_retries is not None and args.baud_retries >= 0:
        BAUD_RETRIES = args.baud_retries
    if args.settle is not None and args.settle >= 0:
        PORT_SETTLE_S = args.settle

    esptool = import_esptool(args.esptool_path)
    ver = getattr(esptool, "__version__", "?")
    cprint(f"esptool fork OK (v{ver}, RP2040Reset present)", style="green")

    if args.list:
        print_boards()
        return 0

    board = resolve_board(args.board) or choose_board_interactive()
    cprint(f"\nSelected: {board['label']}", style="bold")

    if args.dry_run:
        if args.skip_bridge:
            cprint("[dry-run] would skip the bridge UF2 copy (--skip-bridge)",
                   style="yellow")
        else:
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
        cprint(f"[dry-run]   settle: wait for port, then {PORT_SETTLE_S:.1f}s "
               f"before flashing", style="dim")
        return 0

    if args.skip_bridge:
        # Bridge already running: skip the UF2 copy and just find its port. No
        # settle needed since the CDC has long since come up. esptool's own
        # reset (default-reset / RP2040Reset) still drops the ESP into the
        # download loader at connect time.
        cprint("Skipping bridge UF2 copy (--skip-bridge); using the "
               "already-running bridge.", style="cyan")
        if args.port:
            port = args.port
            if not wait_for_port_present(port):
                die(f"serial port {port} is not present. Is the bridge running?")
        else:
            port = find_existing_bridge_port()
        cprint(f"Using serial port {port}", style="dim")
    else:
        # Stage 1: bridge UF2 -> RP mass storage.
        ports_before, _ = stage1_copy_bridge(board)

        # Stage 2a: find the serial bridge port.
        if args.port:
            port = args.port
            cprint(f"Waiting for serial port {port} to appear...", style="cyan")
            if not wait_for_port_present(port):
                die(f"serial port {port} never appeared after the UF2 copy. "
                    f"Is the bridge firmware running?")
            cprint(f"Using serial port {port}", style="dim")
            time.sleep(0.5)
        else:
            cprint("Waiting for the serial bridge to enumerate...", style="cyan")
            port = wait_for_new_serial_port(ports_before)
            if not port:
                die("could not detect a new serial port after the UF2 copy. "
                    "Re-run with --port <device>.")
            cprint(f"  bridge appeared at {port}", style="dim")
            time.sleep(0.5)
        # The device node appears a beat before the freshly-booted bridge CDC is
        # actually ready to talk; flashing too early makes the 921600 baud switch
        # lose its request/ack and forces the slow 115200 fallback. Let it settle.
        if PORT_SETTLE_S > 0:
            cprint(f"Letting the bridge settle ({PORT_SETTLE_S:.1f}s)...",
                   style="dim")
            time.sleep(PORT_SETTLE_S)

    # Stage 2b: flash the ESP, recovering across bridge watchdog resets. If the
    # bridge hangs its watchdog reboots it (re-forcing the ESP into the download
    # loader and re-enumerating USB); we wait for the port to come back and retry
    # the whole flash rather than dying.
    install_progress_logger(esptool)
    for attempt in range(1, BRIDGE_RETRIES + 1):
        try:
            do_flash(esptool, board, port, keep_baud=args.keep_baud)
            cprint("\nESP-NOW firmware flashed OK.", style="green")
            return 0
        except KeyboardInterrupt:
            cprint("\nAborted.", style="yellow")
            return 130
        except Exception as e:  # esptool FatalError, serial disconnect, etc.
            if attempt >= BRIDGE_RETRIES:
                die(f"ESP flash failed after {attempt} attempt(s): {e}")
            cprint(f"\nFlash attempt {attempt}/{BRIDGE_RETRIES} failed ({e}).",
                   style="yellow")
            cprint("Waiting for the bridge to re-enumerate (watchdog reset?)...",
                   style="cyan")
            newport = wait_for_bridge_back(args.port, port)
            if not newport:
                die("bridge did not come back after a reset; aborting.")
            if newport != port:
                cprint(f"  bridge is back at {newport} (was {port})", style="dim")
            port = newport
            if PORT_SETTLE_S > 0:
                time.sleep(PORT_SETTLE_S)   # let the freshly-rebooted CDC settle
    return 0


if __name__ == "__main__":
    sys.exit(main())
