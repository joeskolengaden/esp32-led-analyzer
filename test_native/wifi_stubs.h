/*
 * wifi_stubs.h — native (Mac/Linux) stand-ins for the Arduino/ESP32
 * classes wifi_setup.h depends on: String, Preferences (NVS), WiFi, and
 * the global Serial/millis()/delay() functions. None of these exist off
 * an ESP32 board, so this reimplements just enough of each -- matching
 * real Arduino/esp32-arduino-core behavior for every operation
 * wifi_setup.h actually calls -- to let that exact header compile and run
 * on a dev machine, the same way harness.h does for protocols.h.
 *
 * What this CANNOT verify: real WiFi.scanNetworks()/WiFi.begin() behavior,
 * real NVS flash persistence across a power cycle, or real Serial timing.
 * MockWiFi's scan results and connect outcome are entirely test-controlled
 * fakes -- these tests are only about wifi_setup.h's OWN logic (slot
 * management, line parsing, menu branching), not about whether a real
 * ESP32 radio does what WiFi.h promises.
 *
 * NOT uploaded to the ESP32 -- for `c++ -std=c++17 ...` on a dev machine
 * only. See README.md "Testing off-target".
 */
#pragma once
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <deque>
#include <map>
#include <string>
#include <vector>

// ============================================================================
// String -- just the operations wifi_setup.h actually uses.
// ============================================================================
class String {
public:
    std::string s;
    String() {}
    String(const char* c) : s(c ? c : "") {}
    String(int v) : s(std::to_string(v)) {}

    const char* c_str() const { return s.c_str(); }
    size_t length() const { return s.length(); }

    String& operator+=(char c) { s += c; return *this; }
    String& operator+=(const char* c) { s += c; return *this; }

    char operator[](size_t i) const { return s[i]; }

    bool operator==(const String& o) const { return s == o.s; }
    bool operator==(const char* c) const { return s == c; }

    void trim() {
        size_t a = 0, b = s.length();
        while (a < b && std::isspace((unsigned char)s[a])) a++;
        while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
        s = s.substr(a, b - a);
    }

    int toInt() const { return s.empty() ? 0 : std::atoi(s.c_str()); }
};

static String operator+(const char* lhs, const String& rhs) { return String((std::string(lhs) + rhs.s).c_str()); }

// ============================================================================
// Preferences -- ESP32 NVS stand-in, backed by two in-memory maps. Real NVS
// persists across reboots via flash; this only persists across begin/end
// calls on the SAME object within one test run (== within one simulated
// boot), which is exactly the scope wifi_setup.h's own logic operates in.
// ============================================================================
class Preferences {
public:
    std::map<std::string, int> ints;
    std::map<std::string, std::string> strs;

    void begin(const char*, bool) {}
    void end() {}
    void clear() { ints.clear(); strs.clear(); }

    int getInt(const char* key, int def) {
        auto it = ints.find(key);
        return it == ints.end() ? def : it->second;
    }
    void putInt(const char* key, int v) { ints[key] = v; }

    String getString(const char* key, const char* def) {
        auto it = strs.find(key);
        return it == strs.end() ? String(def) : String(it->second.c_str());
    }
    void putString(const char* key, const String& v) { strs[key] = v.s; }
};

// ============================================================================
// WiFi -- test-controlled fake radio. Tests set scanResults / connectOutcome
// directly before calling into wifi_setup.h; nothing here talks to real
// hardware.
// ============================================================================
#define WIFI_STA 1
#define WL_CONNECTED 3
#define WIFI_AUTH_OPEN 0
#define WIFI_AUTH_WPA2_PSK 3

struct MockNet {
    std::string ssid;
    int32_t rssi;
    int enc;  // WIFI_AUTH_OPEN or anything else = "secured"
};

struct FakeIP {
    std::string ip;
    std::string toString() const { return ip; }
};

class MockWiFiClass {
public:
    std::vector<MockNet> scanResults;
    bool connectOutcome = true;   // what WiFi.status() reports after begin()
    std::string lastBeginSsid, lastBeginPass;
    bool connected = false;

    void mode(int) {}
    void begin(const char* ssid, const char* pass) {
        lastBeginSsid = ssid ? ssid : "";
        lastBeginPass = pass ? pass : "";
        connected = connectOutcome;
    }
    int status() { return connected ? WL_CONNECTED : 0; }
    void disconnect(bool = false) { connected = false; }

    int scanNetworks() { return (int)scanResults.size(); }
    void scanDelete() {}
    String SSID(int i) { return String(scanResults[i].ssid.c_str()); }
    int32_t RSSI(int i) { return scanResults[i].rssi; }
    int encryptionType(int i) { return scanResults[i].enc; }

    FakeIP localIP() { return FakeIP{"192.168.1.99"}; }
};
static MockWiFiClass WiFi;

// ============================================================================
// Serial -- extends the printf/println/print pattern from harness.h with a
// test-fed input queue for available()/read().
// ============================================================================
class HardwareSerial {
public:
    std::string log;
    std::deque<char> input;

    void reset() { log.clear(); input.clear(); }
    void feed(const std::string& s) { for (char c : s) input.push_back(c); }

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

    int available() { return (int)input.size(); }
    int read() {
        if (input.empty()) return -1;
        char c = input.front();
        input.pop_front();
        return (unsigned char)c;
    }
};
static HardwareSerial Serial;

// ============================================================================
// millis()/delay() -- a simulated clock, not real wall-clock time, so a
// "wait for a 10s timeout" test runs in microseconds instead of 10 real
// seconds. Each millis() call advances the fake clock a little on its own
// (mirrors real time passing during a busy-wait loop's own work); delay(ms)
// advances it by exactly ms, matching what a real delay() call promises.
// ============================================================================
static uint64_t g_fakeMillis = 0;
static uint32_t millis() { g_fakeMillis += 5; return (uint32_t)g_fakeMillis; }
static void delay(uint32_t ms) { g_fakeMillis += ms; }

// ============================================================================
// Test result tracking + assertion helpers, matching harness.h's style.
// ============================================================================
static int g_failures = 0;

static void expectContains(HardwareSerial& serial, const char* needle, const char* what) {
    if (serial.log.find(needle) == std::string::npos) {
        g_failures++;
        printf("FAIL %-55s expected to find: \"%s\"\n", what, needle);
        printf("     --- actual output ---\n%s     ---------------------\n", serial.log.c_str());
    } else {
        printf("ok   %s\n", what);
    }
}

static void expectTrue(bool cond, const char* what) {
    if (!cond) {
        g_failures++;
        printf("FAIL %s\n", what);
    } else {
        printf("ok   %s\n", what);
    }
}

static void expectEq(const std::string& got, const std::string& want, const char* what) {
    if (got != want) {
        g_failures++;
        printf("FAIL %-55s got \"%s\", want \"%s\"\n", what, got.c_str(), want.c_str());
    } else {
        printf("ok   %s\n", what);
    }
}
