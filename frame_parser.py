"""
frame_parser.py - turns the ESP32 analyzer's printed frame report (plain
text over serial) into structured data, so host_record.py can give live
PASS/FAIL feedback and save queryable JSON instead of just archiving text.

Regexes are matched against the *exact* Serial.printf/println format
strings in capture_singlewire.h / protocols.h / capture_spi.h /
spi_decoders.h -- see those files if a field stops parsing after a firmware
change; this only ever reads what the firmware already prints, it doesn't
duplicate any of its logic.

Usage: feed it lines one at a time via FrameAccumulator.feed(line); it
returns a parsed dict when a frame is complete, None otherwise.
"""
import re

_FRAME_START_RE = re.compile(r"^---- (single-wire|SPI) frame ----$")
_POLARITY_RE = re.compile(r"^polarity\s*:\s*(.+)$")
_BITS_SW_RE = re.compile(r"^bits\s*:\s*(\d+)\s*\((\d+) bytes\)$")
_BITS_SPI_RE = re.compile(r"^bits\s*:\s*(\d+)\s*\((\d+) whole bytes\)$")
_MATCH_RE = re.compile(r"^  MATCH\s+(.+)$")
_SIG_MATCH_RE = re.compile(r"^  SIGNATURE MATCH\s+(.+?)\s+\((.+)\)$")
_CFG_RE = re.compile(r"^\s+CFG byte = (0x[0-9A-Fa-f]+) -> (.+)$")
_BYTES_RE = re.compile(r"^bytes\s*:(.*)$")
_SPI_BLOCK_RE = re.compile(r"^  -- (.+?) framing check --$")
_SPI_KV_RE = re.compile(r"^\s{4,5}([A-Za-z][A-Za-z0-9/()., ]*?)\s*:\s*(.+)$")


def _parse_single_wire(lines):
    r = {"frame_type": "single-wire", "polarity_raw": None, "inverted": None,
         "bits": None, "byte_count": None, "timing_matches": [],
         "signature_matches": [], "bytes_hex": "", "raw_text": "\n".join(lines)}
    for line in lines:
        m = _POLARITY_RE.match(line)
        if m:
            r["polarity_raw"] = m.group(1)
            r["inverted"] = "INVERTED" in m.group(1)
            continue
        m = _BITS_SW_RE.match(line)
        if m:
            r["bits"] = int(m.group(1))
            r["byte_count"] = int(m.group(2))
            continue
        m = _MATCH_RE.match(line)
        if m:
            r["timing_matches"].append(m.group(1).strip())
            continue
        m = _SIG_MATCH_RE.match(line)
        if m:
            r["signature_matches"].append({"name": m.group(1).strip(), "note": m.group(2).strip()})
            continue
        m = _CFG_RE.match(line)
        if m and r["signature_matches"]:
            r["signature_matches"][-1]["cfg_byte"] = m.group(1)
            r["signature_matches"][-1]["cfg_meaning"] = m.group(2).strip()
            continue
        m = _BYTES_RE.match(line)
        if m:
            r["bytes_hex"] = m.group(1).strip()
    return r


def _parse_spi(lines):
    r = {"frame_type": "SPI", "bits": None, "byte_count": None,
         "bytes_hex": "", "spi_checks": {}, "raw_text": "\n".join(lines)}
    current_block = None
    for line in lines:
        m = _BITS_SPI_RE.match(line)
        if m:
            r["bits"] = int(m.group(1))
            r["byte_count"] = int(m.group(2))
            continue
        m = _BYTES_RE.match(line)
        if m:
            r["bytes_hex"] = m.group(1).strip()
            continue
        m = _SPI_BLOCK_RE.match(line)
        if m:
            current_block = m.group(1).strip()
            r["spi_checks"][current_block] = {}
            continue
        if current_block:
            m = _SPI_KV_RE.match(line)
            if m:
                r["spi_checks"][current_block][m.group(1).strip()] = m.group(2).strip()
    return r


def parse_frame(lines):
    """lines: the full text of one frame report, starting with its
    "---- X frame ----" header line. Returns a parsed dict, or None if
    `lines` doesn't start with a recognized frame header."""
    if not lines:
        return None
    m = _FRAME_START_RE.match(lines[0])
    if not m:
        return None
    if m.group(1) == "single-wire":
        return _parse_single_wire(lines)
    return _parse_spi(lines)


class FrameAccumulator:
    """Feed it lines one at a time (in order) as they arrive from the
    serial port; feed() returns a parsed frame dict when a blank line
    completes one, else None. Lines outside of a frame (the boot banner,
    menu text) are ignored -- returned as None with nothing accumulated."""

    def __init__(self):
        self._buf = []
        self._in_frame = False

    def feed(self, line):
        if _FRAME_START_RE.match(line):
            self._buf = [line]
            self._in_frame = True
            return None
        if self._in_frame:
            if line.strip() == "":
                frame = parse_frame(self._buf)
                self._buf = []
                self._in_frame = False
                return frame
            self._buf.append(line)
        return None
