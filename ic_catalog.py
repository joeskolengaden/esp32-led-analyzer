"""
ic_catalog.py - the known-IC picklist and per-IC expectations, shared between
host_record.py (uses it to auto-pick the analyzer mode, compute an expected
byte count from the LED count you enter, and check captured frames against
it) and list_captures.py (uses it to render the same names consistently).

Names and "timing"/"signature" substrings are deliberately kept in sync with
protocols.h's TIMING_PROFILES/SIGNATURES tables and spi_decoders.h's framing
check labels -- these are matched as substrings against what the firmware
actually printed, not re-implemented independently, so a rename on the
firmware side is the only place this can drift out of sync (and it'll show
up as "no match" rather than a silent wrong match, since substring matching
against a table row that no longer exists just finds nothing).

`bpp` = wire bytes per pixel at 8-bit. `preamble`/`trailer` = fixed literal
bytes added once per port, not per pixel (see fpp-bbb-pixels' decoration-byte
mechanism). `bit_based=True` means the chip isn't byte-granular (SM16716) --
byte-count sanity-checking is skipped for those, since the real check is bits.
"""
import re

# ============================================================================
# Mode codes -- same characters the firmware's serial menu expects (send '1'
# or '2' to pick a capture mode); reused here so host_record.py can
# auto-select the right mode instead of asking the user to type it in twice.
# ============================================================================
SINGLE_WIRE = "1"
SPI = "2"

# ============================================================================
# Known-IC picklist. Each entry's mode/timing/signature/spi_block fields are
# matched (as substrings) against what the firmware actually prints -- see
# the module docstring above for why that's deliberate.
# ============================================================================
IC_CATALOG = {
    "WS2812/WS2811/SK6812 (Normal)": dict(
        mode=SINGLE_WIRE, bpp=3, preamble=0, trailer=0,
        timing="Normal/WS281x-family", signature=None, inverted=False),
    "APA104/APA106/PL9823/SK6822": dict(
        mode=SINGLE_WIRE, bpp=3, preamble=0, trailer=0,
        timing="APA104", signature=None, inverted=False),
    "WS2811 400Kbps": dict(
        mode=SINGLE_WIRE, bpp=3, preamble=0, trailer=0,
        timing="WS2811 400Kbps", signature=None, inverted=False),
    "UCS1903/UCS2903 400Kbps": dict(
        mode=SINGLE_WIRE, bpp=3, preamble=0, trailer=0,
        timing="UCS1903", signature=None, inverted=False),
    "TM1803": dict(
        mode=SINGLE_WIRE, bpp=3, preamble=0, trailer=0,
        timing="TM1803", signature=None, inverted=False),
    "TM1814": dict(
        mode=SINGLE_WIRE, bpp=4, preamble=8, trailer=0,
        timing="TM1814", signature="TM1814", inverted=True),
    "TM1829": dict(
        mode=SINGLE_WIRE, bpp=3, preamble=0, trailer=0,
        timing="TM1829", signature=None, inverted=True),
    "WS2805": dict(
        mode=SINGLE_WIRE, bpp=5, preamble=0, trailer=0,
        timing="WS2805", signature=None, inverted=False),
    "TM1914": dict(
        mode=SINGLE_WIRE, bpp=3, preamble=6, trailer=0,
        timing="TM1914", signature="TM1914", inverted=True),
    "UCS7604 (8-bit)": dict(
        mode=SINGLE_WIRE, bpp=4, preamble=15, trailer=0,
        timing="Normal/WS281x-family", signature="UCS7604", inverted=False),
    "UCS7604 (16-bit)": dict(
        mode=SINGLE_WIRE, bpp=8, preamble=15, trailer=0,
        timing="Normal/WS281x-family", signature="UCS7604", inverted=False),
    "UCS8903 (16-bit RGB)": dict(
        mode=SINGLE_WIRE, bpp=6, preamble=0, trailer=0,
        timing="Normal/WS281x-family", signature=None, inverted=False),
    "UCS8904 (16-bit RGBW)": dict(
        mode=SINGLE_WIRE, bpp=8, preamble=0, trailer=0,
        timing="Normal/WS281x-family", signature=None, inverted=False),
    "SM16825E (16-bit RGBWC)": dict(
        mode=SINGLE_WIRE, bpp=10, preamble=0, trailer=4,
        timing="Normal/WS281x-family", signature="SM16825E", inverted=False),
    "APA102/SK9822/HD107S": dict(
        mode=SPI, bpp=4, preamble=4, trailer=None,  # end frame length varies with pixel count
        spi_block="APA102/SK9822/HD107S", inverted=False),
    "WS2801": dict(
        mode=SPI, bpp=3, preamble=0, trailer=0,
        spi_block="WS2801", inverted=False),
    "P9813": dict(
        mode=SPI, bpp=4, preamble=4, trailer=None,  # end/latch length varies with pixel count (>64px)
        spi_block="P9813", inverted=False),
    "SM16716/SM16726": dict(
        mode=SPI, bpp=3, preamble=0, trailer=0, bit_based=True,
        spi_block="SM16716/SM16726", inverted=False),
}

# Default colour-cycling sequence for the guided workflow. (name, intended RGBW
# tuple) -- W is only meaningful for chips whose catalog entry has bpp
# consistent with a white/extra channel; the recorder just stores whatever
# you confirm, it doesn't reject values based on channel count.
DEFAULT_COLOUR_SEQUENCE = [
    ("red", (255, 0, 0, 0)),
    ("green", (0, 255, 0, 0)),
    ("blue", (0, 0, 255, 0)),
    ("white/extra-channel", (0, 0, 0, 255)),
    ("ascending", (0x11, 0x22, 0x33, 0x44)),
    ("off", (0, 0, 0, 0)),
]


# ============================================================================
# Byte-count sanity check helper, used by host_record.py to compute the
# expected frame size up front (before any capture happens) and compare
# every captured frame against it.
# ============================================================================

def expected_byte_count(ic, led_count):
    """Returns the expected total wire byte count for `ic`'s catalog entry
    at `led_count` pixels, or None if not computable (bit-based chip, a
    variable-length trailer like APA102/P9813's end frame, or a custom/
    "other" IC entered at the prompt with an unknown bytes-per-pixel)."""
    if ic.get("bit_based"):
        return None
    if ic.get("trailer") is None:
        return None
    if ic.get("bpp") is None:
        return None
    return ic["preamble"] + ic["trailer"] + ic["bpp"] * led_count


def catalog_names():
    return list(IC_CATALOG.keys())


# ============================================================================
# Auto-detect scoring: given one already-parsed frame (see frame_parser.py),
# guess which catalog IC it's most likely from and how many LEDs it implies
# -- a ranked, best-effort suggestion for host_record.py's auto-detect
# pre-flight, never a certainty. Evidence used:
#   single-wire: timing-profile match (weak), preamble/trailer signature
#                match (strong), and whether the byte count divides evenly
#                into whole pixels for that IC's bpp (arithmetic sanity).
#   SPI:         the firmware already runs EVERY known SPI chip's framing
#                check against every capture (see spi_decoders.h), so this
#                just reads which block reported the most "OK"s and pulls
#                the LED count straight out of its own "pixel frames found"
#                line -- no guessing of our own beyond picking the block.
# A floating/unconnected pin can still produce a "frame" (noise interpreted
# as edges), so a nonempty result here is a suggestion to confirm, not proof
# a real IC is actually connected.
# ============================================================================

def _spi_pixel_count_guess(info):
    """Pulls the leading integer off a spi_checks sub-block's "pixel frames
    found" value (e.g. "2 (each [...])" -> 2). None if that key is absent
    (WS2801's block has no such line -- see below) or unparseable."""
    val = info.get("pixel frames found")
    if not val:
        return None
    m = re.match(r"\s*(\d+)", val)
    return int(m.group(1)) if m else None


def guess_ics(frame):
    """Returns a list of (name, ic_entry, score, led_count_guess) tuples,
    highest score first, for the given parsed frame dict. Empty list means
    "no real evidence" -- caller should fall back to manual selection."""
    candidates = []
    if frame["frame_type"] == "single-wire":
        for name, ic in IC_CATALOG.items():
            if ic["mode"] != SINGLE_WIRE:
                continue
            score = 0
            led_guess = None
            want_timing = ic.get("timing")
            if want_timing and any(want_timing in m for m in frame["timing_matches"]):
                score += 1
            want_sig = ic.get("signature")
            if want_sig and any(want_sig in s["name"] for s in frame["signature_matches"]):
                score += 3  # a signature match is much stronger evidence than timing alone
            bpp, pre, tr, bc = ic.get("bpp"), ic.get("preamble"), ic.get("trailer"), frame.get("byte_count")
            if bpp and pre is not None and tr is not None and bc is not None:
                rem = bc - pre - tr
                if rem > 0 and rem % bpp == 0:
                    led_guess = rem // bpp
                    score += 1
                else:
                    score -= 2  # byte count doesn't fit this IC's shape at all
            if score > 0:
                candidates.append((name, ic, score, led_guess))
    else:  # SPI -- the firmware already ran every chip's check on this frame
        for name, ic in IC_CATALOG.items():
            if ic["mode"] != SPI:
                continue
            score = 0
            led_guess = None
            block = ic.get("spi_block")
            info = frame.get("spi_checks", {}).get(block, {}) if block else {}
            if name.startswith("WS2801"):
                # WS2801 is a raw passthrough with no framing markers, so its
                # only signal is "byte count divides evenly into RGB triples."
                bc = frame.get("byte_count")
                if bc and bc > 0 and bc % 3 == 0:
                    score += 2
                    led_guess = bc // 3
                else:
                    score -= 2
            else:
                score += sum(1 for v in info.values() if "OK" in v)
                px = _spi_pixel_count_guess(info)
                if px:
                    score += 2
                    led_guess = px
            if score > 0:
                candidates.append((name, ic, score, led_guess))
    candidates.sort(key=lambda c: c[2], reverse=True)
    return candidates
