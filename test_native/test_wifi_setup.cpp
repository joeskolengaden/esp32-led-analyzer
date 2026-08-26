/*
 * test_wifi_setup.cpp — native tests for wifi_setup.h's own logic: the
 * saved-network slot management (append/update-in-place/oldest-eviction),
 * line parsing, and the interactive menu's branching. Uses wifi_stubs.h's
 * fake String/Preferences/WiFi/Serial so the exact header that ships in
 * the sketch runs off-target -- see wifi_stubs.h's header comment for
 * exactly what this can and cannot verify (no real radio, no real flash).
 *
 * Build: c++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined \
 *        -I.. test_wifi_setup.cpp -o /tmp/tw && /tmp/tw
 */
#include "wifi_stubs.h"
#include "../wifi_setup.h"

static void resetAll() {
    Serial.reset();
    wifiPrefs.clear();
    WiFi.scanResults.clear();
    WiFi.connectOutcome = true;
    WiFi.connected = false;
    WiFi.lastBeginSsid.clear();
    WiFi.lastBeginPass.clear();
    g_fakeMillis = 0;
}

int main() {
    // ------------------------------------------------------------------
    printf("-- wifi_save_network / wifi_load_saved: slot management --\n");
    resetAll();
    {
        wifi_save_network("HomeNet", "hunter2");
        SavedWifi out[WIFI_MAX_SAVED];
        int n = wifi_load_saved(out, WIFI_MAX_SAVED);
        expectTrue(n == 1, "one network saved -> load returns 1");
        expectEq(out[0].ssid.s, "HomeNet", "saved SSID round-trips");
        expectEq(out[0].pass.s, "hunter2", "saved password round-trips");
    }

    resetAll();
    {
        wifi_save_network("HomeNet", "firstpass");
        wifi_save_network("HomeNet", "updatedpass");  // same SSID -> update in place
        SavedWifi out[WIFI_MAX_SAVED];
        int n = wifi_load_saved(out, WIFI_MAX_SAVED);
        expectTrue(n == 1, "re-saving the same SSID doesn't grow the count");
        expectEq(out[0].pass.s, "updatedpass", "re-saving the same SSID updates the password in place");
    }

    resetAll();
    {
        wifi_save_network("Net1", "p1");
        wifi_save_network("Net2", "p2");
        wifi_save_network("Net3", "p3");
        wifi_save_network("Net4", "p4");
        wifi_save_network("Net5", "p5");
        SavedWifi out[WIFI_MAX_SAVED];
        int n = wifi_load_saved(out, WIFI_MAX_SAVED);
        expectTrue(n == 5, "five distinct networks saved -> load returns 5");
        expectEq(out[0].ssid.s, "Net1", "slot 0 is the first saved network");
        expectEq(out[4].ssid.s, "Net5", "slot 4 is the fifth saved network");
    }

    resetAll();
    {
        wifi_save_network("Net1", "p1");
        wifi_save_network("Net2", "p2");
        wifi_save_network("Net3", "p3");
        wifi_save_network("Net4", "p4");
        wifi_save_network("Net5", "p5");
        wifi_save_network("Net6", "p6");  // 6th, over the WIFI_MAX_SAVED=5 cap
        SavedWifi out[WIFI_MAX_SAVED];
        int n = wifi_load_saved(out, WIFI_MAX_SAVED);
        expectTrue(n == 5, "adding a 6th network stays capped at 5");
        expectEq(out[0].ssid.s, "Net2", "oldest (Net1) was evicted, Net2 shifted into slot 0");
        expectEq(out[4].ssid.s, "Net6", "newest (Net6) landed in the last slot");
        bool anyIsNet1 = false;
        for (int i = 0; i < n; i++) if (out[i].ssid.s == "Net1") anyIsNet1 = true;
        expectTrue(!anyIsNet1, "the evicted network (Net1) is gone entirely, not just reordered");
    }

    resetAll();
    {
        wifi_save_network("Net1", "p1");
        wifi_forget_all();
        SavedWifi out[WIFI_MAX_SAVED];
        int n = wifi_load_saved(out, WIFI_MAX_SAVED);
        expectTrue(n == 0, "forget_all() clears every saved network");
        expectContains(Serial, "All saved networks forgotten.", "forget_all() reports what it did");
    }

    // ------------------------------------------------------------------
    printf("\n-- wifi_read_line: line assembly and timeout --\n");
    resetAll();
    {
        Serial.feed("hello\n");
        String line = wifi_read_line(200);
        expectEq(line.s, "hello", "a plain LF-terminated line reads back correctly");
    }
    resetAll();
    {
        // A real terminal's Enter often sends \r\n as one pair -- \r must be
        // dropped as part of that SAME keypress, not treated as its own line.
        Serial.feed("hel\rlo\n");
        String line = wifi_read_line(200);
        expectEq(line.s, "hello", "a \\r arriving mid-line is dropped, not inserted into the text");
    }
    resetAll();
    {
        // A bare Enter (just \r\n, no content) must register as an
        // IMMEDIATE empty submission -- this is the actual bug fix: the
        // "[Y/n], Enter = default" prompts in wifi_setup_menu() depend on
        // this returning right away, not after burning the full timeout.
        Serial.feed("\r\n");
        String line = wifi_read_line(200);
        expectEq(line.s, "", "a bare CRLF Enter press returns an empty line immediately");
    }
    resetAll();
    {
        // No input fed at all -- must time out and return empty, not hang.
        String line = wifi_read_line(200);
        expectEq(line.s, "", "no input within the timeout returns an empty string");
    }

    // ------------------------------------------------------------------
    printf("\n-- wifi_setup_menu(): interactive flows --\n");
    resetAll();
    {
        // No input at all at the top-level prompt -> skip cleanly.
        wifi_setup_menu();
        expectContains(Serial, "No input -- skipping WiFi setup.", "no input at the menu skips cleanly");
    }

    resetAll();
    {
        wifi_save_network("HomeNet", "hunter2");
        Serial.feed("1\n");  // pick the one saved network
        WiFi.connectOutcome = true;
        wifi_setup_menu();
        expectEq(WiFi.lastBeginSsid, "HomeNet", "picking a saved network by number connects with its SSID");
        expectEq(WiFi.lastBeginPass, "hunter2", "picking a saved network by number connects with its saved password");
        expectContains(Serial, "[WiFi] Connected. IP:", "a successful saved-network connect reports success");
    }

    resetAll();
    {
        wifi_save_network("Net1", "p1");
        Serial.feed("f\n");
        wifi_setup_menu();
        SavedWifi out[WIFI_MAX_SAVED];
        int n = wifi_load_saved(out, WIFI_MAX_SAVED);
        expectTrue(n == 0, "'f' from the menu forgets every saved network");
    }

    resetAll();
    {
        WiFi.scanResults = {{"OpenCafe", -60, WIFI_AUTH_OPEN}, {"SecureHome", -40, WIFI_AUTH_WPA2_PSK}};
        WiFi.connectOutcome = true;
        Serial.feed("s\n1\ny\n");  // scan, pick #1 (open, no password prompt), then confirm save
        wifi_setup_menu();
        expectContains(Serial, "Networks found:", "scan lists the mock networks");
        expectEq(WiFi.lastBeginSsid, "OpenCafe", "picking an open network from scan connects with that SSID");
        expectEq(WiFi.lastBeginPass, "", "an open network is never prompted for a password");
        SavedWifi out[WIFI_MAX_SAVED];
        int n = wifi_load_saved(out, WIFI_MAX_SAVED);
        expectTrue(n == 1 && out[0].ssid.s == "OpenCafe", "confirming 'y' after a successful connect saves the network");
    }

    resetAll();
    {
        WiFi.scanResults = {{"SecureHome", -40, WIFI_AUTH_WPA2_PSK}};
        WiFi.connectOutcome = false;  // simulate a wrong password / unreachable network
        Serial.feed("s\n1\nwrongpass\n");
        wifi_setup_menu();
        expectContains(Serial, "Failed to connect", "a failed connect attempt is reported as a failure");
        SavedWifi out[WIFI_MAX_SAVED];
        int n = wifi_load_saved(out, WIFI_MAX_SAVED);
        expectTrue(n == 0, "a failed connect is never saved, even without an explicit decline");
        bool asked = Serial.log.find("Save this network") != std::string::npos;
        expectTrue(!asked, "the 'save this network?' prompt never appears after a failed connect");
    }

    resetAll();
    {
        // Bare Enter at "Save this network? [Y/n]" must default to yes,
        // and must do so promptly -- this is the read_line bug fix,
        // exercised end-to-end through the real menu flow.
        WiFi.scanResults = {{"OpenCafe", -60, WIFI_AUTH_OPEN}};
        WiFi.connectOutcome = true;
        Serial.feed("s\n1\n\n");  // scan, pick #1, bare Enter for "save? [Y/n]"
        wifi_setup_menu();
        SavedWifi out[WIFI_MAX_SAVED];
        int n = wifi_load_saved(out, WIFI_MAX_SAVED);
        expectTrue(n == 1 && out[0].ssid.s == "OpenCafe",
                   "a bare Enter at the save-confirmation prompt defaults to yes and saves");
    }

    resetAll();
    {
        WiFi.scanResults.clear();  // scan finds nothing
        Serial.feed("s\n");
        wifi_setup_menu();
        expectContains(Serial, "No networks found.", "an empty scan result is reported, not silently swallowed");
    }

    resetAll();
    {
        Serial.feed("xyz\n");  // not a number, not 's', not 'f', and no saved networks to match anyway
        wifi_setup_menu();
        expectContains(Serial, "Not a valid choice", "unrecognized input falls through to the invalid-choice message");
    }

    resetAll();
    {
        wifi_save_network("OnlyOne", "p");
        Serial.feed("99\n");  // a number, but out of range for the 1 saved network
        wifi_setup_menu();
        expectContains(Serial, "Not a valid choice", "an out-of-range saved-network number is rejected, not out-of-bounds read");
    }

    printf("\n%s\n", g_failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return g_failures == 0 ? 0 : 1;
}
