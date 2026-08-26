"""
color_analysis.py - infers the WIRE channel order (e.g. GRB vs RGB vs RGBW)
from a guided session's own red/green/blue[/white] captures, for
host_record.py to suggest and the user to confirm at the end of a session.

This isn't guessing blind: guided_session() already asks you to set each
primary colour on the BBB and confirm you did it, so by the time all of
red/green/blue (and white, for 4-channel ICs) have been captured, we know
BOTH what channel was supposed to be at full brightness AND what byte
position in the actual wire data came back brightest for that capture. If
those agree consistently across every colour, that's a direct, testable
answer to "what order does this chip actually want its bytes in" -- no
firmware change needed, since capture_singlewire.h already hands over the
full decoded byte stream in `bytes_hex`.

Scope: single-wire only, and only for bpp in (3, 4) -- a plain RGB or RGBW
byte shape where "one channel, one byte, one clearly-brightest position" is
a meaningful question. Higher-bpp chips (16-bit UCS8903/8904, WS2805's 5
channels, SM16825E's 10) don't have a simple single-byte-per-channel
answer, so inferring an order for them would be inventing precision this
method doesn't have -- skipped on purpose, not a gap to close later.
"""

_PRIMARY_TO_LETTER = {"red": "R", "green": "G", "blue": "B", "white/extra-channel": "W"}


def _first_pixel_bytes(segment, bpp):
    """Returns the first pixel's `bpp` bytes (as ints 0-255) from the first
    frame in `segment` that decoded at least that many bytes, or None if no
    frame in the segment has enough data."""
    for frame in segment.get("frames", []):
        hexstr = frame.get("bytes_hex", "") or ""
        try:
            vals = [int(b, 16) for b in hexstr.split()]
        except ValueError:
            continue
        if len(vals) >= bpp:
            return vals[:bpp]
    return None


def infer_color_order(segments, bpp):
    """segments: the guided session's list of segment dicts (as saved in the
    session JSON) -- looks up "red"/"green"/"blue"/"white/extra-channel" by
    name among them. bpp: the selected IC's bytes-per-pixel.

    Returns (order_string, evidence_dict) on a clean, consistent read across
    every required primary colour, e.g. ("GRB", {...}). Returns (None,
    reason_string) if bpp isn't a plain RGB(W) shape, a required colour
    segment is missing/empty, or the evidence is inconsistent (two colours
    pointing at the same wire byte, or no byte clearly dominant) -- in every
    "None" case the reason is meant to be shown to the user, not just logged.
    """
    if bpp not in (3, 4):
        return None, f"bpp={bpp} isn't a plain RGB/RGBW shape (needs exactly 3 or 4 bytes/pixel) -- skipped"

    wanted = ["red", "green", "blue"] + (["white/extra-channel"] if bpp == 4 else [])
    by_colour = {s["colour"]: s for s in segments}

    positions = {}
    evidence = {}
    for colour in wanted:
        seg = by_colour.get(colour)
        if seg is None or not seg.get("frame_count"):
            return None, f"no captured frames for '{colour}' -- can't check that channel"
        px = _first_pixel_bytes(seg, bpp)
        if px is None:
            return None, f"'{colour}' segment's frames didn't decode {bpp} bytes for the first pixel"

        hi_idx = max(range(bpp), key=lambda i: px[i])
        hi_val = px[hi_idx]
        others = sorted((px[i] for i in range(bpp) if i != hi_idx), reverse=True)
        runner_up = others[0] if others else 0
        # Require the winning byte to be both bright in absolute terms and
        # clearly ahead of every other byte in this pixel -- a dim or
        # near-tied capture isn't evidence of anything, it's noise.
        if hi_val < 64 or hi_val <= runner_up + 16:
            return None, (f"'{colour}' segment's brightest byte (0x{hi_val:02X}) isn't clearly "
                           f"dominant over the rest ({[f'0x{v:02X}' for v in px]}) -- ambiguous capture")
        if hi_idx in positions:
            prior = positions[hi_idx]
            return None, (f"both '{colour}' and '{prior}' point to the same wire byte position "
                           f"({hi_idx}) -- inconsistent capture, can't trust either")

        positions[hi_idx] = colour
        evidence[colour] = {"byte_index": hi_idx, "byte_value": hi_val, "pixel_bytes": px}

    # Every wire byte position 0..bpp-1 is guaranteed to have exactly one
    # colour assigned to it at this point: len(wanted) == bpp, and the
    # collision check above already ruled out two colours sharing a
    # position, so it's a one-to-one mapping across every position.
    order = "".join(_PRIMARY_TO_LETTER[positions[i]] for i in range(bpp))
    return order, evidence
