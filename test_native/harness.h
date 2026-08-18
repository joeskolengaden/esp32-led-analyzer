/*
 * harness.h — native (Mac/Linux, non-ESP32) test harness for protocols.h
 * and spi_decoders.h. Neither of those files has any real hardware
 * dependency beyond a printf-capable Serial-like object; this stubs just
 * that, so the exact same headers that ship in the sketch compile and run
 * off-target.
 *
 * NOT uploaded to the ESP32 -- for `c++ -std=c++17 ...` on a dev machine
 * only. See README.md "Testing off-target".
 */
#pragma once
#include <cstdarg>
#include <cstdio>
#include <string>

class HardwareSerial {
public:
    std::string log;
    void reset() { log.clear(); }
    int printf(const char* fmt, ...) {
        char tmp[1024];
        va_list args;
        va_start(args, fmt);
        int n = vsnprintf(tmp, sizeof(tmp), fmt, args);
        va_end(args);
        log += tmp;
        return n;
    }
    void println(const char* s) { log += s; log += "\n"; }
    void println() { log += "\n"; }
    void print(const char* s) { log += s; }
};

static int g_failures = 0;

// Asserts `needle` appears somewhere in `serial.log`; prints a diff-style
// failure if not.
static void expectContains(HardwareSerial& serial, const char* needle, const char* what) {
    if (serial.log.find(needle) == std::string::npos) {
        g_failures++;
        printf("FAIL %-55s expected to find: \"%s\"\n", what, needle);
        printf("     --- actual output ---\n%s     ---------------------\n", serial.log.c_str());
    } else {
        printf("ok   %s\n", what);
    }
}

static void expectNotContains(HardwareSerial& serial, const char* needle, const char* what) {
    if (serial.log.find(needle) != std::string::npos) {
        g_failures++;
        printf("FAIL %-55s expected NOT to find: \"%s\"\n", what, needle);
        printf("     --- actual output ---\n%s     ---------------------\n", serial.log.c_str());
    } else {
        printf("ok   %s\n", what);
    }
}
