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

SINGLE_WIRE = "1"
SPI = "2"

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


def expected_byte_count(ic, led_count):
    """Returns the expected total wire byte count for `ic`'s catalog entry
    at `led_count` pixels, or None if not computable (bit-based chip, or a
    variable-length trailer like APA102/P9813's end frame)."""
    if ic.get("bit_based"):
        return None
    if ic.get("trailer") is None:
        return None
    return ic["preamble"] + ic["trailer"] + ic["bpp"] * led_count


def catalog_names():
    return list(IC_CATALOG.keys())
