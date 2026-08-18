// Synthetic end-to-end pipeline test for protocols.h -- feeds classify_timing()
// and match_signatures() the actual values our own patches produce (not
// hand-picked numbers), so it catches integration bugs a compile check or an
// isolated unit test can't: an off-by-one window edge, a signature that
// never matches because of an index mistake, an unintended cross-match.
//
//   c++ -std=c++17 -I.. test_protocols.cpp -o /tmp/test_protocols && /tmp/test_protocols
#include "harness.h"
#include "../protocols.h"
#include <cstring>

static void test_timing_profiles_match_their_own_midpoint() {
    printf("\n-- every timing profile matches its own reference midpoint --\n");
    for (int i = 0; i < NUM_TIMING_PROFILES; i++) {
        const TimingProfile& p = TIMING_PROFILES[i];
        int t0mid = (p.t0MinNs + p.t0MaxNs) / 2;
        int t1mid = (p.t1MinNs + p.t1MaxNs) / 2;
        int pmid  = (p.periodMinNs + p.periodMaxNs) / 2;
        HardwareSerial s;
        classify_timing(p.inverted, t0mid, t1mid, pmid, s);
        char what[128];
        snprintf(what, sizeof(what), "profile matches itself: %s", p.name);
        char matchLine[160];
        snprintf(matchLine, sizeof(matchLine), "MATCH   %s", p.name);
        expectContains(s, matchLine, what);
    }
}

static void test_boundary_edges() {
    printf("\n-- window boundaries: exact min/max edges match, just outside doesn't --\n");
    // Normal/WS281x-family (0): T0 200-550, T1 580-1050, period 1000-1500.
    const TimingProfile& normal = TIMING_PROFILES[0];
    {
        HardwareSerial s;
        // exactly at every min edge -- inRange() is inclusive, must match.
        classify_timing(false, normal.t0MinNs, normal.t1MinNs, normal.periodMinNs, s);
        expectContains(s, "MATCH   Normal/WS281x-family (0)", "T0/T1/period all at exact MIN edge matches");
    }
    {
        HardwareSerial s;
        classify_timing(false, normal.t0MaxNs, normal.t1MaxNs, normal.periodMaxNs, s);
        expectContains(s, "MATCH   Normal/WS281x-family (0)", "T0/T1/period all at exact MAX edge matches");
    }
    {
        HardwareSerial s;
        // 1ns below the T0 floor -- must NOT match Normal specifically.
        classify_timing(false, normal.t0MinNs - 1, normal.t1MinNs, normal.periodMinNs, s);
        expectNotContains(s, "MATCH   Normal/WS281x-family (0)", "T0 1ns below MIN does not match Normal");
    }
    {
        HardwareSerial s;
        classify_timing(false, normal.t0MaxNs + 1, normal.t1MaxNs, normal.periodMaxNs, s);
        expectNotContains(s, "MATCH   Normal/WS281x-family (0)", "T0 1ns above MAX does not match Normal");
    }
}

static void test_tm1814_tm1914_share_window_by_design() {
    printf("\n-- TM1814/TM1914 share identical windows -- both matching is BY DESIGN, not a bug --\n");
    // Confirmed identical in protocols.h; only their preamble differs. This
    // test documents that expectation so it can't silently regress into
    // "only one matches" (a real bug) without someone noticing here.
    const TimingProfile& tm1814 = TIMING_PROFILES[5]; // "TM1814 (6, inverted)"
    if (strcmp(tm1814.name, "TM1814 (6, inverted)") != 0) {
        g_failures++;
        printf("FAIL test assumes TIMING_PROFILES[5] is TM1814 -- table order changed, fix this test's index\n");
        return;
    }
    int t0mid = (tm1814.t0MinNs + tm1814.t0MaxNs) / 2;
    int t1mid = (tm1814.t1MinNs + tm1814.t1MaxNs) / 2;
    int pmid  = (tm1814.periodMinNs + tm1814.periodMaxNs) / 2;
    HardwareSerial s;
    classify_timing(true, t0mid, t1mid, pmid, s);
    expectContains(s, "MATCH   TM1814 (6, inverted)", "TM1814-typical values match TM1814 profile");
    expectContains(s, "MATCH   TM1914 (9, inverted)", "TM1814-typical values ALSO match TM1914 profile (shared window)");
    // TM1829's window also genuinely overlaps TM1814's at these values (both
    // Titan-family inverted chips with close datasheet timing) -- confirmed
    // by hand against the actual numbers in protocols.h, not a test bug.
    // This is why the signature scan, not timing alone, is what actually
    // tells TM1814 (has a preamble) apart from TM1829 (never has one).
    expectContains(s, "MATCH   TM1829 (7, inverted)", "TM1814-typical values ALSO match TM1829 (overlapping window -- signature is the real disambiguator)");
}

static void test_no_match_reports_nearest_candidates() {
    printf("\n-- a value inside no window reports nearest candidates, not silence --\n");
    HardwareSerial s;
    // Wildly out of range for every profile.
    classify_timing(false, 50, 50, 100, s);
    expectContains(s, "No timing profile matched", "no-match case says so explicitly");
}

static void test_signatures_detected() {
    printf("\n-- every shipped signature is detected at its actual position --\n");
    {
        uint8_t buf[12] = { 0xFF,0xFF,0xFF,0xFF, 0x00,0x00,0x00,0x00, 0x10,0x11,0x12,0x13 }; // TM1814 preamble + a pixel
        HardwareSerial s;
        match_signatures(buf, sizeof(buf), s);
        expectContains(s, "TM1814 8-byte current preamble", "TM1814 preamble detected at start");
    }
    {
        uint8_t buf[9] = { 0xFF,0xFF,0xF5, 0x00,0x00,0x0A, 0x10,0x11,0x12 }; // TM1914 preamble + a pixel
        HardwareSerial s;
        match_signatures(buf, sizeof(buf), s);
        expectContains(s, "TM1914 6-byte mode preamble", "TM1914 preamble detected at start");
    }
    {
        // UCS7604 8-bit: CFG=0x03
        uint8_t buf8[15] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x02, 0x03, 0x08,0x08,0x08,0x08, 0x00,0x00 };
        HardwareSerial s8;
        match_signatures(buf8, sizeof(buf8), s8);
        expectContains(s8, "UCS7604 8-byte sync", "UCS7604 sync detected (8-bit case)");
        expectContains(s8, "CFG byte = 0x03 -> 8-bit/800kbps/RGBW", "UCS7604 CFG=0x03 decodes to 8-bit");
        // UCS7604 16-bit: CFG=0x8B
        uint8_t buf16[15] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x02, 0x8B, 0x08,0x08,0x08,0x08, 0x00,0x00 };
        HardwareSerial s16;
        match_signatures(buf16, sizeof(buf16), s16);
        expectContains(s16, "CFG byte = 0x8B -> 16-bit/800kbps/RGBW", "UCS7604 CFG=0x8B decodes to 16-bit");
    }
    {
        // SM16825E trailer: must be detected at the END, not the start, of
        // an otherwise-unrelated buffer (it's a trailer, not a preamble).
        uint8_t buf[14] = { 0x10,0x10,0x11,0x11,0x12,0x12,0x13,0x13,0x14,0x14, 0x29,0x4A,0x52,0x9F };
        HardwareSerial s;
        match_signatures(buf, sizeof(buf), s);
        expectContains(s, "SM16825E current-gain trailer", "SM16825E trailer detected at end");
        // Sanity: the same 4 trailer bytes prepended instead of appended
        // must NOT register as a (nonsensical) "trailer at the start" match
        // -- match_signatures only ever checks the position each Signature
        // entry declares.
        uint8_t bufWrongPos[14] = { 0x29,0x4A,0x52,0x9F, 0x10,0x10,0x11,0x11,0x12,0x12,0x13,0x13,0x14,0x14 };
        HardwareSerial s2;
        match_signatures(bufWrongPos, sizeof(bufWrongPos), s2);
        expectNotContains(s2, "SM16825E current-gain trailer", "SM16825E trailer bytes at the START (wrong position) do not match");
    }
    {
        // Plain pixel data, no preamble/trailer -- must report none found,
        // not a false positive.
        uint8_t buf[9] = { 0x10,0x20,0x30, 0x40,0x50,0x60, 0x70,0x80,0x90 };
        HardwareSerial s;
        match_signatures(buf, sizeof(buf), s);
        expectContains(s, "no known preamble/trailer signature found", "plain pixel data: no false-positive signature match");
    }
}

int main() {
    test_timing_profiles_match_their_own_midpoint();
    test_boundary_edges();
    test_tm1814_tm1914_share_window_by_design();
    test_no_match_reports_nearest_candidates();
    test_signatures_detected();
    printf(g_failures ? "\n%d FAILURE(S)\n" : "\nALL TESTS PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
