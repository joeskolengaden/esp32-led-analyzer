#!/usr/bin/env python3
"""
host_record.py - guided, interactive recording of the ESP32 LED Signal
Analyzer's captures, tagged with IC name + LED count and cycled through a
colour sequence you confirm as you set each one on the BBB -- so a saved
session says what chip and colour every captured frame belongs to, not just
"here's some serial output from some time." Runs on the HOST (not the
ESP32); no firmware changes, no WiFi, no credentials on the device.

Guided mode (default):
    .venv/bin/python3 host_record.py -p /dev/cu.usbserial-0001

Old passive "just record everything, no prompts" mode is still available:
    .venv/bin/python3 host_record.py -p /dev/cu.usbserial-0001 --freeform --label whatever

List ports / captured sessions:
    .venv/bin/python3 host_record.py --list
    .venv/bin/python3 list_captures.py
"""
import argparse
import datetime
import re
import sys
import threading
import time
from pathlib import Path

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit(
        "pyserial is required. From this directory:\n"
        "  python3 -m venv .venv && .venv/bin/pip install pyserial\n"
        "then re-run with .venv/bin/python3 host_record.py ..."
    )

from ic_catalog import IC_CATALOG, SINGLE_WIRE, SPI, DEFAULT_COLOUR_SEQUENCE, expected_byte_count, catalog_names, guess_ics
from frame_parser import FrameAccumulator

DEFAULT_BAUD = 115200
_ANSI_RE = re.compile(rb"\x1b\[[0-9;]*[a-zA-Z]")  # strips any future colour codes from device output


# ============================================================================
# Serial port discovery
# ============================================================================

def list_ports():
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        print("No serial ports found.")
        return
    print("Available serial ports:")
    for p in ports:
        print(f"  {p.device}   {p.description}")


# ============================================================================
# Wiring safety gate. This is NOT a substitute for actual overvoltage
# protection -- no firmware or host-side code can make an ESP32-S3 GPIO pin
# (3.3V absolute max) safe against a genuine 5V signal; that's a hardware
# fact, not a software one. What this CAN do is make sure nobody starts a
# capture without having deliberately thought about it, and make clear that
# "I don't have a level shifter" usually isn't actually a blocker: a plain
# 1k/2k resistor divider does the same job as a commercial level-shifter
# module for a few cents, and some BBB cape designs expose a 3.3V-logic tap
# point before their own onboard buffer/level-shifter chip -- meaning zero
# extra parts may be needed at all if you can wire to that point instead.
# ============================================================================

def confirm_wiring_safety():
    print("\n" + "!" * 70)
    print("! ESP32-S3 GPIO is 3.3V ONLY. A bare 5V data/clock line WILL damage it.")
    print("! You do NOT need a commercial level-shifter module for this -- any")
    print("! one of these is enough, cheapest first:")
    print("!   1) Tap a 3.3V-logic point in the signal chain instead, if your")
    print("!      cape/board exposes the driver's output BEFORE its own 5V")
    print("!      buffer chip (check your board's schematic/silkscreen).")
    print("!   2) A 2-resistor divider: 1k from the source to the GPIO pin,")
    print("!      2k from that same pin to GND. ~$0.02 in parts, no chip needed.")
    print("!   3) A real level-shifter module (74HCT245, TXB0108, etc.) if you")
    print("!      have one -- functionally equivalent to (2) for this purpose.")
    print("! Wiring 5V straight into GPIO4/5/6 with none of the above WILL risk")
    print("! frying the board. See the README's Wiring section for details.")
    print("!" * 70)
    ans = input("Type 'yes' to confirm your signal is 3.3V-safe (shifted, divided, or\n"
                "already 3.3V logic) and continue: ").strip().lower()
    if ans != "yes":
        sys.exit("\nStopped -- wiring not confirmed. Nothing was opened or captured.")


# ============================================================================
# Small interactive-prompt helpers, shared by both guided and freeform modes
# ============================================================================

def _ask(prompt, default=None):
    suffix = f" [{default}]" if default is not None else ""
    val = input(f"{prompt}{suffix}: ").strip()
    return val if val else default


def _ask_int(prompt, default=None):
    while True:
        raw = _ask(prompt, str(default) if default is not None else None)
        try:
            n = int(raw)
            if n <= 0:
                print("  Enter a positive number.")
                continue
            return n
        except (TypeError, ValueError):
            print("  Not a number, try again.")


# ============================================================================
# IC picklist prompt
# ============================================================================

def prompt_ic_selection():
    """Shows the numbered IC_CATALOG list, plus an "other" option for a
    custom name. Returns (name, catalog_dict) either way -- a custom entry
    gets a synthesized dict with unknown fields set to None/defaults so the
    rest of the code (expected_byte_count, verdict_line) can treat it
    exactly like a real catalog entry."""
    names = catalog_names()
    print("\nKnown ICs:")
    for i, n in enumerate(names, 1):
        mode_label = "single-wire" if IC_CATALOG[n]["mode"] == SINGLE_WIRE else "SPI"
        print(f"  {i:2}) {n}  ({mode_label})")
    print(f"  {len(names) + 1:2}) other (type a custom name)")
    while True:
        choice = input(f"Pick an IC [1-{len(names) + 1}]: ").strip()
        if choice.isdigit():
            idx = int(choice)
            if 1 <= idx <= len(names):
                name = names[idx - 1]
                return name, IC_CATALOG[name]
            if idx == len(names) + 1:
                custom = input("Custom IC name: ").strip() or "unknown"
                mode = _ask("Mode: (1) single-wire or (2) SPI", "1")
                mode = SINGLE_WIRE if mode not in (SINGLE_WIRE, SPI) else mode
                return custom, {"mode": mode, "bpp": None, "preamble": 0, "trailer": 0,
                                 "timing": None, "signature": None, "inverted": False,
                                 "spi_block": None}
        print("  Not a valid choice, try again.")


# ============================================================================
# Auto-detect pre-flight: before asking the user to pick an IC and enter an
# LED count by hand, briefly listen on the wire and try to guess both from
# what's actually there. This is a suggestion to confirm, not a verdict --
# a floating/unwired pin can still produce a "frame" out of noise, and
# timing/signature matches are inherently ambiguous for some chip families
# (see protocols.h's TM1814/TM1829/TM1914 overlap). The user always gets the
# final say, exactly like every other pass/fail check in this tool.
# ============================================================================

def _listen_for_frame(ser, acc, seconds):
    """Reads from `ser` for up to `seconds`, feeding each line to `acc`.
    Returns the first complete frame dict, or None if nothing completed
    within the time budget."""
    deadline = time.time() + seconds
    buf = b""
    while time.time() < deadline:
        chunk = ser.read(256)
        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            line = _ANSI_RE.sub(b"", line).rstrip(b"\r")
            frame = acc.feed(line.decode("utf-8", errors="replace"))
            if frame is not None:
                return frame
    return None


def auto_detect(port_name, baud, seconds_per_mode=3.0):
    """Tries single-wire mode first, then SPI mode, waiting up to
    `seconds_per_mode` in each for a real frame to arrive. Returns
    (ser, mode_str, frame) and leaves `ser` OPEN if something was found --
    the caller should hand that same connection to guided_session() rather
    than reopening (reopening risks a board reset undoing the mode we just
    selected, depending on the board's auto-reset wiring). Returns
    (None, None, None), with the port already closed, if nothing arrived in
    either mode -- e.g. nothing is wired up yet."""
    print(f"\nListening for a signal ({seconds_per_mode:.0f}s per mode, single-wire then SPI)...")
    try:
        ser = serial.Serial(port_name, baud, timeout=0.2)
    except serial.SerialException as e:
        print(f"  Couldn't open {port_name} for auto-detect: {e}")
        return None, None, None

    time.sleep(0.3)  # let the board finish printing its boot banner/menu
    ser.reset_input_buffer()
    ser.write(SINGLE_WIRE.encode())
    frame = _listen_for_frame(ser, FrameAccumulator(), seconds_per_mode)
    if frame is not None:
        return ser, "single-wire", frame

    ser.write(b" ")  # any key while capturing returns the device to its menu
    time.sleep(0.1)
    ser.reset_input_buffer()
    ser.write(SPI.encode())
    frame = _listen_for_frame(ser, FrameAccumulator(), seconds_per_mode)
    if frame is not None:
        return ser, "SPI", frame

    ser.close()
    return None, None, None


def prompt_confirm_detection(mode_str, frame):
    """Shows guess_ics()'s ranked candidates for a detected frame and lets
    the user accept the top guess, pick a different one, adjust the LED
    count, or bail to the manual picklist. Returns (ic_name, ic, led_count)
    on acceptance, or None to signal "fall back to manual"."""
    candidates = guess_ics(frame)
    print(f"\nDetected a signal in {mode_str} mode.")
    if not candidates:
        print("  Couldn't confidently match it to a known IC from timing/signature alone.")
        return None
    shown = candidates[:5]
    print("  Best guesses (ranked -- confirm before recording, this is not certain):")
    for i, (name, ic, score, led_guess) in enumerate(shown, 1):
        led_txt = f", ~{led_guess} LEDs" if led_guess else ""
        print(f"    {i}) {name}{led_txt}")
    choice = input("  [Enter]=use #1, or a number, or m=manual picklist: ").strip().lower()
    if choice == "m":
        return None
    if choice == "":
        idx = 0
    elif choice.isdigit() and 1 <= int(choice) <= len(shown):
        idx = int(choice) - 1
    else:
        print("  Not a valid choice -- falling back to the manual picklist.")
        return None
    name, ic, _, led_guess = shown[idx]
    led_count = _ask_int("LED / pixel count in the test string", led_guess or 1)
    return name, ic, led_count


# ============================================================================
# Per-frame pass/fail verdict, computed against the selected IC's expectations
# ============================================================================

def verdict_line(frame, ic, expected_bytes):
    """One compact line summarizing whether a parsed frame matches what
    `ic`'s catalog entry expects. Returns (line, passed)."""
    checks = []
    passed = True
    if frame["frame_type"] == "single-wire":
        if expected_bytes is not None:
            ok = frame["byte_count"] == expected_bytes
            passed &= ok
            checks.append(f"bytes {frame['byte_count']}/{expected_bytes} {'OK' if ok else 'MISMATCH'}")
        else:
            checks.append(f"bytes {frame['byte_count']}")
        want_timing = ic.get("timing")
        if want_timing:
            ok = any(want_timing in m for m in frame["timing_matches"])
            passed &= ok
            checks.append(f"timing {'OK' if ok else 'NO MATCH'}")
        if frame["inverted"] is not None and "inverted" in ic:
            ok = frame["inverted"] == ic["inverted"]
            passed &= ok
            checks.append(f"polarity {'OK' if ok else 'WRONG'}")
        want_sig = ic.get("signature")
        if want_sig:
            ok = any(want_sig in s["name"] for s in frame["signature_matches"])
            passed &= ok
            checks.append(f"signature {'OK' if ok else 'NOT FOUND'}")
    else:
        checks.append(f"bytes {frame['byte_count']}")
        block = ic.get("spi_block")
        info = frame["spi_checks"].get(block, {}) if block else {}
        for k, v in info.items():
            if "OK" in v or "MISSING" in v or "WRONG" in v:
                ok = "OK" in v
                passed &= ok
                checks.append(f"{k.split('(')[0].strip()} {'OK' if ok else 'FAIL'}")
    return "    frame: " + " | ".join(checks), passed


# ============================================================================
# Shared state between the main thread (drives prompts, owns colour timing)
# and the reader thread (parses serial as it arrives). active_segment is the
# handoff point: None means "not capturing, ignore parsed frames"; a list
# means "capturing, append parsed+verdict-tagged frames here." Access to it
# is always under `lock` since both threads touch it.
# ============================================================================

class SessionState:
    def __init__(self):
        self.active_segment = None  # list, or None when not capturing
        self.lock = threading.Lock()


# ============================================================================
# Guided session: the main interactive recording flow. Opens the port, tells
# the device which mode to run, then walks the user through the default
# colour sequence (with repeat/skip/add-custom/quit options at each step),
# saving a .log (raw timestamped serial text) and a .json (structured,
# queryable session data) no matter how the session ends -- including Ctrl+C.
# ============================================================================

def guided_session(port_name, baud, out_dir, ic_name, ic, led_count, notes, ser=None, mode_confirmed=False):
    import json

    # ---- Setup: filenames, serial port, log header, mode select ----
    out_dir.mkdir(parents=True, exist_ok=True)
    ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    safe_ic = re.sub(r"[^A-Za-z0-9_-]+", "-", ic_name).strip("-") or "ic"
    session_id = f"{ts}_{safe_ic}"
    logpath = out_dir / f"session_{session_id}.log"
    jsonpath = out_dir / f"session_{session_id}.json"

    # `ser` may already be open and in the right mode, handed off from
    # auto_detect()'s pre-flight sniff -- reopening here instead would risk
    # a board reset undoing the mode select we already confirmed.
    if ser is None:
        print(f"\nOpening {port_name} @ {baud} baud...")
        try:
            ser = serial.Serial(port_name, baud, timeout=0.2)
        except serial.SerialException as e:
            sys.exit(f"Couldn't open {port_name}: {e}\n"
                      f"Check the device is plugged in, the path is right (--list to see options), "
                      f"and no other program (Arduino Serial Monitor, another host_record.py) has it open.")
    start = datetime.datetime.now()
    log = open(logpath, "w", encoding="utf-8", errors="replace")
    log.write(f"# ESP32 LED Signal Analyzer -- guided session\n# ic: {ic_name}\n"
              f"# led_count: {led_count}\n# started: {start.isoformat()}\n"
              f"# port: {port_name}\n# baud: {baud}\n#{'-' * 60}\n")
    log.flush()

    mode_name = "single-wire" if ic["mode"] == SINGLE_WIRE else "SPI"
    exp_bytes = expected_byte_count(ic, led_count)
    if mode_confirmed:
        print(f"Already listening in {mode_name} mode from auto-detect -- continuing without re-selecting.")
    else:
        print(f"Selecting {mode_name} mode on the device (sending '{ic['mode']}')...")
        ser.write(ic["mode"].encode())
    if ic["mode"] == SPI:
        print("Reminder: temporarily lower spiSpeed to ~200000 in your FPP config for this test.")
    if exp_bytes is not None:
        print(f"Expecting {exp_bytes} bytes/frame for {led_count}x {ic_name} "
              f"({ic['preamble']} preamble + {ic['bpp']}*{led_count} pixel + {ic['trailer']} trailer).")

    # ---- Reader thread: continuously parses serial lines into frames and,
    # while a colour is actively being captured, tags each frame with a
    # verdict and appends it to the shared segment list ----
    state = SessionState()
    acc = FrameAccumulator()
    stop_flag = threading.Event()

    def reader():
        buf = b""
        while not stop_flag.is_set():
            try:
                chunk = ser.read(256)
            except serial.SerialException:
                break
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = _ANSI_RE.sub(b"", line).rstrip(b"\r")
                text = line.decode("utf-8", errors="replace")
                elapsed = (datetime.datetime.now() - start).total_seconds()
                log.write(f"[{elapsed:8.3f}s] {text}\n")
                log.flush()
                frame = acc.feed(text)
                if frame is not None:
                    with state.lock:
                        if state.active_segment is not None:
                            line_out, passed = verdict_line(frame, ic, exp_bytes)
                            frame["_passed"] = passed
                            print(line_out)
                            state.active_segment.append(frame)

    reader_thread = threading.Thread(target=reader, daemon=True)
    reader_thread.start()

    # Everything from here on must run inside try/finally: if the user hits
    # Ctrl+C mid-colour (or anything else goes wrong), we still want to save
    # whatever segments -- including a partially-captured in-progress one --
    # were good, rather than silently losing already-captured frames. This
    # is the difference between "recorded 5 colours, Ctrl+C'd on the 6th"
    # losing nothing vs. losing the whole session.
    segments = []
    interrupted = False
    try:
        # ---- Colour-cycling loop: prompt to set a colour on the BBB, wait
        # for Enter, capture until the next Enter, record the segment, then
        # ask what to do next (advance/repeat/skip/add custom/quit) ----
        colours = list(DEFAULT_COLOUR_SEQUENCE)
        i = 0
        while i < len(colours):
            colour_name, rgbw = colours[i]
            print(f"\n==> Set the {ic_name} string to {colour_name.upper()} "
                  f"(R={rgbw[0]},G={rgbw[1]},B={rgbw[2]},W={rgbw[3]}) on the BBB, "
                  f"then press Enter to start capturing.")
            input()
            seg_start = datetime.datetime.now()
            with state.lock:
                state.active_segment = []
            print(f"  Capturing '{colour_name}'... press Enter again once you've seen enough frames.")
            input()
            with state.lock:
                frames = state.active_segment
                state.active_segment = None
            seg_end = datetime.datetime.now()
            pass_count = sum(1 for f in frames if f.get("_passed"))
            print(f"  -> {len(frames)} frame(s) captured, {pass_count}/{len(frames)} matched expectations.")
            segments.append({
                "colour": colour_name, "intended_rgbw": list(rgbw),
                "started": seg_start.isoformat(), "ended": seg_end.isoformat(),
                "frame_count": len(frames), "pass_count": pass_count, "frames": frames,
            })
            choice = input("  [Enter]=next colour, r=repeat this colour, "
                            "s=skip remaining defaults, a=add a custom colour, q=end session: ").strip().lower()
            if choice == "r":
                continue  # redo same index
            if choice == "s":
                break
            if choice == "a":
                name = input("    Custom colour name: ").strip() or f"custom{len(colours)}"
                r = _ask_int("    R", 0)
                g = _ask_int("    G", 0)
                b = _ask_int("    B", 0)
                w = _ask_int("    W (0 if n/a)", 0)
                colours.insert(i + 1, (name, (r, g, b, w)))
            if choice == "q":
                break
            i += 1
    except KeyboardInterrupt:
        interrupted = True
        # If Ctrl+C landed while a colour was actively capturing, save
        # whatever frames it already had rather than discarding them.
        with state.lock:
            in_progress = state.active_segment
            state.active_segment = None
        if in_progress:
            pass_count = sum(1 for f in in_progress if f.get("_passed"))
            now = datetime.datetime.now().isoformat()
            segments.append({
                "colour": "(interrupted)", "intended_rgbw": None,
                "started": now, "ended": now, "frame_count": len(in_progress),
                "pass_count": pass_count, "frames": in_progress,
            })
        print("\nInterrupted -- saving what was captured so far.")
    finally:
        # ---- Finalize: always runs, however the loop above exited --
        # stop the reader thread, close the port, write the log footer and
        # the JSON sidecar, print a pass/fail summary per colour ----
        stop_flag.set()
        ser.close()
        end = datetime.datetime.now()
        duration = (end - start).total_seconds()
        total_frames = sum(s["frame_count"] for s in segments)
        total_pass = sum(s["pass_count"] for s in segments)

        log.write(f"#{'-' * 60}\n# ended: {end.isoformat()}\n# duration: {duration:.1f}s\n"
                  f"# interrupted: {interrupted}\n"
                  f"# colours tested: {len(segments)}\n# frames: {total_frames} ({total_pass} passed)\n")
        log.close()

        session = {
            "session_id": session_id, "ic_name": ic_name, "mode": mode_name,
            "led_count": led_count, "notes": notes, "port": port_name, "baud": baud,
            "started": start.isoformat(), "ended": end.isoformat(), "duration_s": duration,
            "interrupted": interrupted,
            "expected_bytes_per_frame": exp_bytes, "segments": segments,
        }
        with open(jsonpath, "w", encoding="utf-8") as jf:
            json.dump(session, jf, indent=2)

        print(f"\n{'=' * 60}\nSession summary: {ic_name}  ({led_count} LEDs, {mode_name})"
              f"{'  [INTERRUPTED]' if interrupted else ''}")
        for s in segments:
            verdict = "OK" if s["frame_count"] and s["pass_count"] == s["frame_count"] else \
                      ("NO FRAMES" if not s["frame_count"] else "SOME FAILED")
            print(f"  {s['colour']:<22} {s['pass_count']}/{s['frame_count']} frames  {verdict}")
        print(f"Saved: {logpath}\n        {jsonpath}")


# ============================================================================
# Freeform mode (--freeform): the original passive recorder, kept for ad-hoc
# capture that doesn't fit the guided IC/colour structure. No IC metadata,
# no verdicts -- just raw timestamped serial text plus optional keystroke
# forwarding so you can still drive the device's 1/2 mode menu while it
# records.
# ============================================================================

def freeform_record(port_name, baud, out_dir, label):
    """The original passive mode: record everything, no prompts, no
    per-IC/colour structure. Still useful for ad-hoc/exploratory capture."""
    out_dir.mkdir(parents=True, exist_ok=True)
    ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    fname = f"session_{ts}"
    if label:
        safe_label = re.sub(r"[^A-Za-z0-9_-]+", "-", label).strip("-")
        if safe_label:
            fname += f"_{safe_label}"
    logpath = out_dir / f"{fname}.log"

    print(f"Opening {port_name} @ {baud} baud...")
    ser = serial.Serial(port_name, baud, timeout=0.2)
    start = datetime.datetime.now()
    log = open(logpath, "w", encoding="utf-8", errors="replace")
    log.write("# ESP32 LED Signal Analyzer -- freeform session\n"
              f"# started : {start.isoformat()}\n# port    : {port_name}\n# baud    : {baud}\n")
    if label:
        log.write(f"# label   : {label}\n")
    log.write("#" + "-" * 60 + "\n")
    log.flush()

    print(f"Recording to {logpath}")
    print("Type 1/2 to pick a capture mode on the device, any key to return to")
    print("its menu, Ctrl+C here to stop recording.\n")

    stop_flag = threading.Event()
    line_count = 0

    def reader():
        nonlocal line_count
        buf = b""
        while not stop_flag.is_set():
            try:
                chunk = ser.read(256)
            except serial.SerialException:
                break
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = _ANSI_RE.sub(b"", line).rstrip(b"\r")
                text = line.decode("utf-8", errors="replace")
                elapsed = (datetime.datetime.now() - start).total_seconds()
                sys.stdout.write(text + "\n")
                sys.stdout.flush()
                log.write(f"[{elapsed:8.3f}s] {text}\n")
                log.flush()
                line_count += 1

    def writer():
        if not sys.stdin.isatty():
            return
        import tty
        import termios
        fd = sys.stdin.fileno()
        try:
            old = termios.tcgetattr(fd)
        except termios.error:
            return
        try:
            tty.setcbreak(fd)
            while not stop_flag.is_set():
                ch = sys.stdin.read(1)
                if ch:
                    ser.write(ch.encode())
        except Exception:
            pass
        finally:
            termios.tcsetattr(fd, termios.TCSADRAIN, old)

    reader_thread = threading.Thread(target=reader, daemon=True)
    writer_thread = threading.Thread(target=writer, daemon=True)
    reader_thread.start()
    if sys.stdin.isatty():
        writer_thread.start()
    else:
        print("(stdin isn't an interactive terminal -- recording only, no keystroke forwarding)")

    try:
        while reader_thread.is_alive():
            time.sleep(0.2)
    except KeyboardInterrupt:
        pass
    finally:
        stop_flag.set()
        end = datetime.datetime.now()
        duration = (end - start).total_seconds()
        log.write("#" + "-" * 60 + "\n")
        log.write(f"# ended    : {end.isoformat()}\n# duration : {duration:.1f}s\n# lines    : {line_count}\n")
        log.close()
        ser.close()
        print(f"\nStopped. {line_count} lines over {duration:.1f}s -> {logpath}")


# ============================================================================
# CLI entry point: --list to enumerate ports, --freeform for the passive
# recorder, otherwise the guided flow (default) -- pick an IC, enter LED
# count and notes, then hand off to guided_session().
# ============================================================================

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--port", help="serial device, e.g. /dev/cu.usbserial-0001")
    ap.add_argument("-b", "--baud", type=int, default=DEFAULT_BAUD)
    ap.add_argument("-o", "--out-dir", default="captures", help="directory to write session files into")
    ap.add_argument("--list", action="store_true", help="list available serial ports and exit")
    ap.add_argument("--freeform", action="store_true", help="old passive mode: no prompts, just record everything")
    ap.add_argument("-l", "--label", help="(--freeform only) short label appended to the filename")
    ap.add_argument("--no-detect", action="store_true",
                     help="skip the auto-detect sniff, go straight to the manual IC/LED-count prompts")
    ap.add_argument("--detect-seconds", type=float, default=3.0,
                     help="how long to listen per mode during auto-detect (default 3s)")
    ap.add_argument("--wiring-confirmed", action="store_true",
                     help="skip the 3.3V wiring-safety prompt (once you've confirmed it for this setup)")
    args = ap.parse_args()

    if args.list:
        list_ports()
        return
    if not args.port:
        list_ports()
        sys.exit("\nPass -p/--port with one of the ports above.")

    if not args.wiring_confirmed:
        confirm_wiring_safety()

    if args.freeform:
        freeform_record(args.port, args.baud, Path(args.out_dir), args.label)
        return

    print("=== Guided capture session ===")

    ser = None
    ic_name = ic = led_count = None
    mode_confirmed = False
    try:
        if not args.no_detect:
            ser, mode_str, frame = auto_detect(args.port, args.baud, args.detect_seconds)
            if frame is not None:
                result = prompt_confirm_detection(mode_str, frame)
                if result is not None:
                    ic_name, ic, led_count = result
                    mode_confirmed = True
                else:
                    ser.close()  # declined the guess -- let guided_session open a fresh connection
                    ser = None
            else:
                print("  No signal detected in either mode -- check wiring, or the string "
                      "isn't being driven yet. Falling back to manual selection.")
    except KeyboardInterrupt:
        if ser is not None:
            ser.close()
        sys.exit("\nCancelled before any capture started -- nothing was recorded.")

    if ic is None:
        ic_name, ic = prompt_ic_selection()
        led_count = _ask_int("LED / pixel count in the test string", 1)

    notes = input("Session notes (optional, Enter to skip): ").strip()
    guided_session(args.port, args.baud, Path(args.out_dir), ic_name, ic, led_count, notes,
                   ser=ser, mode_confirmed=mode_confirmed)


if __name__ == "__main__":
    main()
