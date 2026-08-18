// Round-trip tests for spi_decoders.h against the exact known-good encoded
// vectors already verified in plugins/SPIPixels/tests/test_format.cpp --
// same principle as the SM16716 decode check done earlier in this session,
// extended to APA102/P9813/WS2801, plus empty-buffer safety for all four
// (run under ASan/UBSan to catch any out-of-bounds read on a short/empty
// capture, which is exactly the kind of input a noisy real capture produces).
//
//   c++ -std=c++17 -fsanitize=address,undefined -I.. test_spi_decoders.cpp -o /tmp/t && /tmp/t
#include "harness.h"
#include "../spi_decoders.h"

static void test_apa102_2px() {
    printf("\n-- APA102 2px (from SPIPixels test_format.cpp) --\n");
    // start(4x00) + [FF c0 c1 c2] x2 + 4x FF end
    uint8_t buf[16] = { 0x00,0x00,0x00,0x00,
                         0xFF,0x0A,0x14,0x1E,
                         0xFF,0x28,0x32,0x3C,
                         0xFF,0xFF,0xFF,0xFF };
    HardwareSerial s;
    spi_check_apa102(buf, sizeof(buf), "APA102/SK9822/HD107S", s);
    expectContains(s, "start frame (4x0x00): OK", "APA102 start frame recognized");
    expectContains(s, "pixel frames found   : 2", "APA102 pixel count = 2");
    expectContains(s, "end frame (4 bytes, all 0xFF): OK", "APA102 end frame recognized");
    expectContains(s, "brightness=31/31, BGR=0A 14 1E", "APA102 first pixel decoded correctly");
}

static void test_apa102_end_frame_not_misread_as_pixels() {
    printf("\n-- APA102 trailing all-0xFF end frame is not misread as extra pixels --\n");
    // 20 real (non-white) pixels + a 4-group (16-byte) end frame, per
    // 4*ceil(20/16)=8... use ceil(20/16)=2 groups=8 bytes to also confirm a
    // multi-group end frame (not just the single-group case) is fully
    // peeled back, not just the last group of it.
    int nPix = 20, endGroups = 2; // 4*ceil(20/16) = 8 bytes = 2 groups
    int n = 4 + nPix * 4 + endGroups * 4;
    uint8_t* buf = new uint8_t[n];
    for (int i = 0; i < 4; i++) buf[i] = 0x00;
    for (int p = 0; p < nPix; p++) {
        int o = 4 + p * 4;
        buf[o] = 0xE5; buf[o+1] = 0x11; buf[o+2] = 0x22; buf[o+3] = 0x33; // not white
    }
    for (int g = 0; g < endGroups; g++) {
        int o = 4 + nPix * 4 + g * 4;
        buf[o] = buf[o+1] = buf[o+2] = buf[o+3] = 0xFF;
    }
    HardwareSerial s;
    spi_check_apa102(buf, n, "APA102", s);
    char want[64]; snprintf(want, sizeof(want), "pixel frames found   : %d", nPix);
    expectContains(s, want, "correct pixel count, end frame not counted as pixels");
    char wantEnd[64]; snprintf(wantEnd, sizeof(wantEnd), "end frame (%d bytes, all 0xFF): OK", endGroups * 4);
    expectContains(s, wantEnd, "multi-group end frame fully recognized, not left partially consumed");
    delete[] buf;
}

static void test_p9813_1px() {
    printf("\n-- P9813 1px (from SPIPixels test_format.cpp) --\n");
    // start(4x00) + [flag=F1 c0=00 c1=FF c2=80] + end(4x00)
    uint8_t buf[12] = { 0x00,0x00,0x00,0x00,
                         0xF1,0x00,0xFF,0x80,
                         0x00,0x00,0x00,0x00 };
    HardwareSerial s;
    spi_check_p9813(buf, sizeof(buf), s);
    expectContains(s, "start frame (4x0x00): OK", "P9813 start frame recognized");
    expectContains(s, "pixel frames found   : 1", "P9813 pixel count = 1");
    expectContains(s, "flag=0xF1, BGR=00 FF 80", "P9813 first pixel decoded correctly");
}

static void test_p9813_end_frame_scales_past_64px() {
    printf("\n-- P9813 end-frame length scaling (this session's bug fix) --\n");
    // 65 pixels: start(4) + 65*4 pixel bytes + 8-byte end (per the fix:
    // 4*ceil(65/64)=8, not the old fixed 4).
    int n = 4 + 65 * 4 + 8;
    uint8_t* buf = new uint8_t[n];
    memset(buf, 0, n);
    for (int i = 0; i < 4; i++) buf[i] = 0x00; // start
    for (int p = 0; p < 65; p++) {
        int o = 4 + p * 4;
        buf[o] = 0xC0; // valid flag (top 2 bits set), rest arbitrary-but-not-0xC0-prefixed
        buf[o+1] = 0x11; buf[o+2] = 0x22; buf[o+3] = 0x33;
    }
    HardwareSerial s;
    spi_check_p9813(buf, n, s);
    expectContains(s, "pixel frames found   : 65", "P9813 correctly walks past the 64px boundary");
    expectContains(s, "end/latch bytes      : 8", "P9813 end-frame length reflects the >64px scaling fix");
    delete[] buf;
}

static void test_ws2801_passthrough() {
    printf("\n-- WS2801 passthrough (from SPIPixels test_format.cpp) --\n");
    uint8_t buf[6] = { 1, 2, 3, 4, 5, 6 };
    HardwareSerial s;
    spi_check_ws2801(buf, sizeof(buf), s);
    expectContains(s, "6 bytes captured, divisible by 3 (RGB triples) OK", "WS2801 byte-count check");
    expectContains(s, "first pixel RGB: 01 02 03", "WS2801 first pixel decoded correctly");
}

static void test_ws2801_bad_length() {
    printf("\n-- WS2801 non-multiple-of-3 length is flagged, not silently accepted --\n");
    uint8_t buf[7] = { 1, 2, 3, 4, 5, 6, 7 };
    HardwareSerial s;
    spi_check_ws2801(buf, sizeof(buf), s);
    expectContains(s, "NOT divisible by 3", "WS2801 flags a bad byte count");
}

static void test_empty_buffers_dont_crash() {
    printf("\n-- empty/too-short captures: no crash, no out-of-bounds read (run under ASan) --\n");
    uint8_t dummy[1] = { 0 };
    HardwareSerial s1; spi_check_apa102(dummy, 0, "APA102", s1);
    expectContains(s1, "too short", "APA102 handles n=0 safely");
    HardwareSerial s2; spi_check_p9813(dummy, 0, s2);
    expectContains(s2, "too short", "P9813 handles n=0 safely");
    HardwareSerial s3; spi_check_ws2801(dummy, 0, s3);
    expectContains(s3, "0 bytes captured", "WS2801 handles n=0 safely (reports 0 bytes, not a crash)");
    expectNotContains(s3, "OK", "WS2801 n=0 does not falsely report the vacuous divisible-by-3 case as OK");
    HardwareSerial s4; spi_check_sm16716(dummy, 0, s4);
    expectContains(s4, "too short", "SM16716 handles nBits=0 safely");
}

int main() {
    test_apa102_2px();
    test_apa102_end_frame_not_misread_as_pixels();
    test_p9813_1px();
    test_p9813_end_frame_scales_past_64px();
    test_ws2801_passthrough();
    test_ws2801_bad_length();
    test_empty_buffers_dont_crash();
    printf(g_failures ? "\n%d FAILURE(S)\n" : "\nALL TESTS PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
