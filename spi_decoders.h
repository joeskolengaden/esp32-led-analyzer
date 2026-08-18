/*
 * spi_decoders.h — pure frame-shape checkers for the clocked SPI chips
 * SPIPixels drives. No Arduino/hardware dependencies beyond Serial (passed
 * by reference, so it can be swapped for a test stub) -- this mirrors
 * protocols.h's testability, and is what capture_spi.h's ISR-based capture
 * hands its byte/bit buffer to once a frame is done.
 */
#pragma once
#include <stdint.h>

static inline int spi_getBit(const uint8_t* buf, uint32_t bitIdx) {
    return (buf[bitIdx / 8] >> (7 - (bitIdx % 8))) & 1;
}

// Byte-oriented checks (APA102/WS2801/P9813 are all byte-aligned).
static void spi_check_apa102(const uint8_t* b, int n, const char* label, HardwareSerial& out) {
    out.printf("  -- %s framing check --\n", label);
    if (n < 4 + 4) { out.println("     too short to be a valid frame"); return; }
    bool sofOk = true;
    for (int i = 0; i < 4; i++) if (b[i] != 0x00) sofOk = false;
    out.printf("     start frame (4x0x00): %s\n", sofOk ? "OK" : "MISSING/WRONG");
    int i = 4, px = 0;
    while (i + 4 <= n && (b[i] & 0xE0) == 0xE0) { i += 4; px++; }
    // A pixel header byte is 0xE0-0xFF (top 3 bits set) -- but so is every
    // byte of the canonical all-0xFF end frame, so a run of {FF,FF,FF,FF}
    // groups just walked as "pixels" is structurally indistinguishable from
    // max-brightness white pixels UNLESS we know whether the run is
    // followed by more real data or runs straight to the end of the
    // capture. Only when the walk consumed every remaining byte (i==n, i.e.
    // nothing unconsumed for an end frame to occupy) do we peel trailing
    // {FF,FF,FF,FF} groups back off as the end frame instead -- ALL of
    // them, not just one, since a real end frame can be several groups long
    // (4*ceil(pixels/16) bytes). A genuine white pixel in the *middle* of
    // the stream is unaffected either way.
    // Residual, inherent ambiguity: a deliberate all-white test pattern
    // with no trailing end frame at all will be misread as "0 pixels, all
    // end frame" -- there is no byte-level way to tell those apart without
    // knowing the expected pixel count in advance.
    if (i == n) {
        while (px > 0 && b[i-4] == 0xFF && b[i-3] == 0xFF && b[i-2] == 0xFF && b[i-1] == 0xFF) {
            i -= 4; px--;
        }
    }
    out.printf("     pixel frames found   : %d (each [0xEx|brightness][B][G][R])\n", px);
    int endLen = n - i;
    bool endOk = endLen > 0;
    for (int j = i; j < n; j++) if (b[j] != 0xFF) endOk = false;
    out.printf("     end frame (%d bytes, all 0xFF): %s\n", endLen, endOk ? "OK" : "MISSING/WRONG");
    if (px > 0) {
        out.printf("     first pixel: brightness=%d/31, BGR=%02X %02X %02X\n",
                   b[4] & 0x1F, b[5], b[6], b[7]);
    }
}

static void spi_check_p9813(const uint8_t* b, int n, HardwareSerial& out) {
    out.println("  -- P9813 framing check --");
    if (n < 4 + 4) { out.println("     too short to be a valid frame"); return; }
    bool sofOk = true;
    for (int i = 0; i < 4; i++) if (b[i] != 0x00) sofOk = false;
    out.printf("     start frame (4x0x00): %s\n", sofOk ? "OK" : "MISSING/WRONG");
    int i = 4, px = 0;
    while (i + 4 <= n && (b[i] & 0xC0) == 0xC0) { i += 4; px++; }
    out.printf("     pixel frames found   : %d (each [flag][B][G][R], flag top 2 bits set)\n", px);
    // end frame length now scales with px per the P9813 fix this session
    // (4 * max(1, ceil(px/64))) -- just report what's left over.
    int endLen = n - i;
    out.printf("     end/latch bytes      : %d (expect >=4, scaling with pixel count past 64px)\n", endLen);
    if (px > 0) out.printf("     first pixel: flag=0x%02X, BGR=%02X %02X %02X\n", b[4], b[5], b[6], b[7]);
}

static void spi_check_ws2801(const uint8_t* b, int n, HardwareSerial& out) {
    out.println("  -- WS2801 framing check --");
    out.printf("     %d bytes captured, %s\n", n,
               (n % 3 == 0 && n > 0) ? "divisible by 3 (RGB triples) OK" : "NOT divisible by 3 -- check byte count");
    if (n >= 3) out.printf("     first pixel RGB: %02X %02X %02X\n", b[0], b[1], b[2]);
}

// SM16716/SM16726: bit-level, not byte-aligned. 50 zero start bits, then per
// pixel: 1 marker bit ('1') + 24-bit RGB MSB-first. See SPIChipFormat.h.
static void spi_check_sm16716(const uint8_t* b, uint32_t nBits, HardwareSerial& out) {
    out.println("  -- SM16716/SM16726 framing check --");
    if (nBits < 50) { out.println("     too short to be a valid frame"); return; }
    uint32_t zeros = 0;
    while (zeros < nBits && spi_getBit(b, zeros) == 0) zeros++;
    out.printf("     leading zero bits: %u (expect ~50)\n", (unsigned)zeros);
    uint32_t pos = zeros, px = 0;
    while (pos + 25 <= nBits && spi_getBit(b, pos) == 1) {
        uint32_t r = 0, g = 0, bch = 0;
        for (int k = 0; k < 8; k++) r = (r << 1) | spi_getBit(b, pos + 1 + k);
        for (int k = 0; k < 8; k++) g = (g << 1) | spi_getBit(b, pos + 9 + k);
        for (int k = 0; k < 8; k++) bch = (bch << 1) | spi_getBit(b, pos + 17 + k);
        if (px == 0) out.printf("     first pixel RGB: %02X %02X %02X\n", (int)r, (int)g, (int)bch);
        pos += 25;
        px++;
    }
    out.printf("     pixel frames found: %u (each 1 marker bit + 24-bit RGB)\n", (unsigned)px);
}
