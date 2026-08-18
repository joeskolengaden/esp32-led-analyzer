#!/usr/bin/env python3
"""
host_record.py - records the ESP32 LED Signal Analyzer's serial output to a
timestamped local log file, so a capture session can be reviewed or diffed
later instead of only ever being read live off the terminal.

This runs on the HOST machine (not the ESP32) and just taps the USB-serial
stream the sketch already prints to -- no firmware changes, no WiFi, no
credentials on the device itself. That's a deliberate choice: the analyzer
is always tethered over USB during a capture session (that's how you talk to
it -- send '1'/'2' to pick a mode), so host-side capture gets you "record
everything for later" with zero added attack surface on the device.

Usage:
    .venv/bin/python3 host_record.py --list                 # show serial ports
    .venv/bin/python3 host_record.py -p /dev/cu.usbserial-0001
    .venv/bin/python3 host_record.py -p /dev/cu.usbserial-0001 --label tm1814-scope-check

Keystrokes typed at this terminal are forwarded to the ESP32 (so you can still
send '1'/'2' to pick a capture mode, or any key to return to its menu).
Ctrl+C stops the recording cleanly and writes a session footer.
"""
import argparse
import datetime
import re
import sys
import threading
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit(
        "pyserial is required. From this directory:\n"
        "  python3 -m venv .venv && .venv/bin/pip install pyserial\n"
        "then re-run with .venv/bin/python3 host_record.py ..."
    )

DEFAULT_BAUD = 115200
# Strip ANSI escape codes if a future sketch revision ever adds color -- the
# current one doesn't, but this keeps recorded logs plain-text either way.
_ANSI_RE = re.compile(rb"\x1b\[[0-9;]*[a-zA-Z]")


def list_ports():
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        print("No serial ports found.")
        return
    print("Available serial ports:")
    for p in ports:
        print(f"  {p.device}   {p.description}")


def record(port_name, baud, out_dir, label):
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
    log.write("# ESP32 LED Signal Analyzer -- recorded session\n")
    log.write(f"# started : {start.isoformat()}\n")
    log.write(f"# port    : {port_name}\n")
    log.write(f"# baud    : {baud}\n")
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
        # Forwards keystrokes typed at this terminal to the ESP32 (menu
        # selection, "return to menu" keypress) -- unbuffered, one char at a
        # time, same as a normal serial monitor. Only meaningful when stdin
        # is a real interactive terminal; skip quietly otherwise (piped,
        # backgrounded, or run from non-interactive tooling) instead of
        # crashing on tcgetattr's ioctl failing against a non-TTY fd.
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
        log.write(f"# ended    : {end.isoformat()}\n")
        log.write(f"# duration : {duration:.1f}s\n")
        log.write(f"# lines    : {line_count}\n")
        log.close()
        ser.close()
        print(f"\nStopped. {line_count} lines over {duration:.1f}s -> {logpath}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--port", help="serial device, e.g. /dev/cu.usbserial-0001")
    ap.add_argument("-b", "--baud", type=int, default=DEFAULT_BAUD)
    ap.add_argument("-o", "--out-dir", default="captures", help="directory to write session logs into")
    ap.add_argument("-l", "--label", help="short label appended to the filename, e.g. tm1814-scope-check")
    ap.add_argument("--list", action="store_true", help="list available serial ports and exit")
    args = ap.parse_args()

    if args.list:
        list_ports()
        return

    if not args.port:
        list_ports()
        sys.exit("\nPass -p/--port with one of the ports above.")

    from pathlib import Path
    record(args.port, args.baud, Path(args.out_dir), args.label)


if __name__ == "__main__":
    main()
