/*
 * protocols.h — known-good timing windows and byte signatures for every
 * single-wire protocol this session's fpp-bbb-pixels work touches.
 *
 * Windows are LIBRARY-CONSENSUS windows (what a correctly-working
 * implementation actually emits), not raw datasheet min/max — a
 * datasheet-strict checker would false-flag a correctly working BBB, per
 * the "Datasheet vs. Silicon" research this tool exists to close the loop
 * on. Where a preset was newly added this session (TM1829/TM1914/WS2805/
 * UCS7604) the window is derived from the same PRU-define-minus-overhead
 * convention as the rest of fpp-bbb-pixels' presets: see
 * fpp-bbb-pixels/REFERENCE.md §4 for the nominal chip specs these are
 * built from, and IC_IMPLEMENTATION_STUDY.md §4.3 for the TM1814 margin
 * discussion this table's TM1814/TM1829/TM1914 rows are based on.
 */
#pragma once
#include <stdint.h>
#include <string.h>

// One capture matches a TimingProfile if: T0/T1 (the active-phase duration
// for a 0-bit / 1-bit) and the total bit period all fall in range, and the
// idle-polarity matches. `resetMinUs` is a floor, not a window -- reset gaps
// vary a lot and only "too short" is diagnostic.
struct TimingProfile {
    const char* name;
    bool inverted;        // true = idles HIGH (TM1814/TM1829/TM1914)
    int t0MinNs, t0MaxNs;  // 0-bit active-phase duration
    int t1MinNs, t1MaxNs;  // 1-bit active-phase duration
    int periodMinNs, periodMaxNs;
    int resetMinUs;
    const char* pixelTiming; // matching fpp-bbb-pixels Pixel Timing dropdown value
};

static const TimingProfile TIMING_PROFILES[] = {
    // -- Normal / ws2811-equivalent family (preset 0): covers WS2812B,
    //    WS2811-800, WS2813, WS2815, SK6812, GS8208, UCS8903/8904(16-bit,
    //    same wire timing), UCS7604(preset 10, same wire timing), and
    //    ~30 more aliases FPP treats as one 800kHz timing. Windows per the
    //    earlier "Datasheet vs. Silicon" research (library consensus, not
    //    raw WS2812B datasheet numbers).
    { "Normal/WS281x-family (0)",     false, 200, 550,  580, 1050, 1000, 1500,  200, "0" },
    { "APA104/106/PL9823/SK6822 (2)", false, 250, 450, 1200, 1520, 1550, 1870,   24, "2" },
    { "WS2811 400Kbps (3)",           false, 350, 650, 1050, 1350, 2350, 2650,   50, "3" },
    { "UCS1903/2903 400Kbps (4)",     false, 350, 650, 1850, 2150, 2350, 2650,   24, "4" },
    { "TM1803 (5)",                   false, 530, 830, 1210, 1510, 1890, 2190,   24, "5" },
    // -- Inverted family: idles HIGH, decoder reports the LOW-phase
    //    duration as "active". TM1814/TM1914 share wire timing exactly
    //    (only their preamble differs -- see SIGNATURES below). TM1829's
    //    window also genuinely OVERLAPS TM1814's (both Titan-family chips
    //    with similar datasheet timing, confirmed by test_native's pipeline
    //    test) -- a capture landing in the shared T1 650-950/period
    //    1100-1400 region will MATCH BOTH. That's expected, not a bug:
    //    timing alone can't tell them apart there. The signature scan is
    //    the real disambiguator -- TM1814 always carries its 8-byte current
    //    preamble, TM1829 never does.
    { "TM1814 (6, inverted)",         true,  310, 410,  650, 1000, 1100, 1400,  200, "6" },
    { "TM1829 (7, inverted)",         true,  200, 400,  650,  950, 1100, 1400,  180, "7" },
    { "TM1914 (9, inverted)",         true,  310, 410,  650, 1000, 1100, 1400,  200, "9" },
    // -- WS2805: not inverted, tighter than Normal (own PRU preset 8).
    { "WS2805 (8)",                   false, 150, 450,  640,  940,  940, 1240,   50, "8" },
};
static const int NUM_TIMING_PROFILES = sizeof(TIMING_PROFILES) / sizeof(TIMING_PROFILES[0]);

// Signature bytes we know to look for in a decoded stream -- preambles at
// the front, the SM16825E trailer at the back. Matching a signature is
// independent of (and stronger evidence than) timing classification alone:
// timing says "this could be several chips in this family," a signature
// match says "this specific frame is chip X."
enum SigPos { SIG_START, SIG_END };
struct Signature {
    const char* name;
    SigPos pos;
    const uint8_t* bytes;
    int len;
    const char* note;
};

static const uint8_t SIG_TM1814[]  = { 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t SIG_TM1914[]  = { 0xFF, 0xFF, 0xF5, 0x00, 0x00, 0x0A };
// UCS7604's 8-byte sync is fixed; the config byte after it is 0x03 (8-bit)
// or 0x8B (16-bit) and the 4 current bytes are user-configurable, so this
// signature only covers the fixed sync -- the decoder reports the config
// byte separately once the sync matches (see match_signatures()).
static const uint8_t SIG_UCS7604_SYNC[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x02 };
// SM16825E current-gain trailer at fpp-bbb-pixels' shipped default (gain
// index 5 of 0-31, ~58mA/channel) -- see patch_pixelstring.py, "SAFETY".
// If you changed `g` there, update this to match or the trailer just won't
// signature-match (timing/period checks for the pixel data itself are
// unaffected).
static const uint8_t SIG_SM16825E_TRAILER[] = { 0x29, 0x4A, 0x52, 0x9F };

static const Signature SIGNATURES[] = {
    { "TM1814 8-byte current preamble",  SIG_START, SIG_TM1814,         8, "expect at bytes 0-7" },
    { "TM1914 6-byte mode preamble",     SIG_START, SIG_TM1914,         6, "expect at bytes 0-5" },
    { "UCS7604 8-byte sync (of 15-byte config)", SIG_START, SIG_UCS7604_SYNC, 8,
      "config byte follows at offset 8: 0x03=8-bit/800k/RGBW, 0x8B=16-bit/800k/RGBW" },
    { "SM16825E current-gain trailer (default g=5)", SIG_END, SIG_SM16825E_TRAILER, 4,
      "~58mA/channel; edit patch_pixelstring.py's `g` if you changed the default" },
};
static const int NUM_SIGNATURES = sizeof(SIGNATURES) / sizeof(SIGNATURES[0]);

// Returns true if [lo,hi] fully contains [wantLo,wantHi]... no -- simpler:
// true if `v` falls in [lo,hi].
static inline bool inRange(int v, int lo, int hi) { return v >= lo && v <= hi; }

// Classifies a capture against every timing profile; prints every match
// (several may match -- that's expected, e.g. Normal/UCS7604/UCS8903 share
// timing) plus near-misses within 20% to help spot a marginal signal.
static void classify_timing(bool inverted, int t0Ns, int t1Ns, int periodNs, HardwareSerial& out) {
    bool anyMatch = false;
    for (int i = 0; i < NUM_TIMING_PROFILES; i++) {
        const TimingProfile& p = TIMING_PROFILES[i];
        if (p.inverted != inverted) continue;
        bool t0ok = inRange(t0Ns, p.t0MinNs, p.t0MaxNs);
        bool t1ok = inRange(t1Ns, p.t1MinNs, p.t1MaxNs);
        bool pOk  = inRange(periodNs, p.periodMinNs, p.periodMaxNs);
        if (t0ok && t1ok && pOk) {
            out.printf("  MATCH   %s\n", p.name);
            anyMatch = true;
        }
    }
    if (!anyMatch) {
        out.println("  No timing profile matched within its window. Nearest candidates:");
        for (int i = 0; i < NUM_TIMING_PROFILES; i++) {
            const TimingProfile& p = TIMING_PROFILES[i];
            if (p.inverted != inverted) continue;
            int t0mid = (p.t0MinNs + p.t0MaxNs) / 2, t1mid = (p.t1MinNs + p.t1MaxNs) / 2;
            int dt0 = t0Ns - t0mid, dt1 = t1Ns - t1mid;
            int pct0 = t0mid ? (dt0 * 100 / t0mid) : 0, pct1 = t1mid ? (dt1 * 100 / t1mid) : 0;
            if (abs(pct0) <= 30 && abs(pct1) <= 30) {
                out.printf("    close: %-28s T0 off %+d%%, T1 off %+d%%\n", p.name, pct0, pct1);
            }
        }
    }
}

// Scans a decoded byte buffer for known preamble/trailer signatures.
static void match_signatures(const uint8_t* bytes, int nBytes, HardwareSerial& out) {
    bool any = false;
    for (int i = 0; i < NUM_SIGNATURES; i++) {
        const Signature& s = SIGNATURES[i];
        if (nBytes < s.len) continue;
        const uint8_t* window = (s.pos == SIG_START) ? bytes : (bytes + nBytes - s.len);
        if (memcmp(window, s.bytes, s.len) == 0) {
            out.printf("  SIGNATURE MATCH  %s (%s)\n", s.name, s.note);
            if (s.bytes == SIG_UCS7604_SYNC && nBytes > 8) {
                uint8_t cfg = bytes[8];
                out.printf("    CFG byte = 0x%02X -> %s\n", cfg,
                           cfg == 0x8B ? "16-bit/800kbps/RGBW" :
                           cfg == 0x03 ? "8-bit/800kbps/RGBW" : "unrecognized (check current bytes too)");
            }
            any = true;
        }
    }
    if (!any) out.println("  (no known preamble/trailer signature found -- fine for plain pixel data)");
}
