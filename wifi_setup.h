/*
 * wifi_setup.h -- optional WiFi join over the Serial console: scan
 * networks, let the user pick one (or a previously-saved one) and type a
 * password, connect, and remember it via Preferences (ESP32 NVS) for next
 * boot. Zero third-party libraries -- WiFi.h and Preferences.h both ship
 * as part of the esp32 Arduino core, same as driver/rmt_rx.h elsewhere in
 * this sketch.
 *
 * Only entered by explicitly sending 'w' from the boot menu (see
 * esp32-led-analyzer.ino's print_menu()/loop()) -- it is NEVER triggered
 * automatically at boot. That's deliberate: host_record.py sends '1' or
 * '2' immediately after opening the serial port on every single capture
 * session, with no idea a WiFi option exists. An auto-prompt here would
 * eat that first keystroke and break the analyzer's core interface on
 * every connection, not just the ones where you actually want WiFi.
 *
 * *** Two things worth knowing before you use this: ***
 *  1. Credentials are stored in plain ESP32 NVS (Preferences), NOT
 *     encrypted at rest unless you've separately enabled ESP32 flash
 *     encryption (out of scope here -- that's a whole-device provisioning
 *     step, not something this sketch turns on for you). This bench tool
 *     now carries your WiFi password if it's lost, stolen, or handed to
 *     someone else -- the exact tradeoff this project avoided by design
 *     until now (see README's "Why host-side, not on-device"). Use the
 *     'f' (forget all) option before lending the board out, or don't save
 *     credentials you'd mind losing.
 *  2. WiFi's radio activity can add interrupt latency that perturbs
 *     nanosecond-precision RMT captures (a documented ESP32 WiFi/RMT
 *     coexistence effect, not specific to this sketch). If a capture looks
 *     unexpectedly jittery, try disconnecting WiFi (or power-cycling
 *     without joining a network) before re-testing.
 */
#pragma once
#include <WiFi.h>
#include <Preferences.h>

#define WIFI_MAX_SAVED          5
#define WIFI_CONNECT_TIMEOUT_MS 15000

static Preferences wifiPrefs;

struct SavedWifi {
    String ssid;
    String pass;
};

// ============================================================================
// Storage: up to WIFI_MAX_SAVED networks in one Preferences namespace.
// ============================================================================

static int wifi_load_saved(SavedWifi* out, int maxN) {
    wifiPrefs.begin("wifi", true);  // read-only
    int n = wifiPrefs.getInt("count", 0);
    if (n > maxN) n = maxN;
    for (int i = 0; i < n; i++) {
        out[i].ssid = wifiPrefs.getString(("ssid" + String(i)).c_str(), "");
        out[i].pass = wifiPrefs.getString(("pass" + String(i)).c_str(), "");
    }
    wifiPrefs.end();
    return n;
}

static void wifi_save_network(const String& ssid, const String& pass) {
    SavedWifi existing[WIFI_MAX_SAVED];
    int n = wifi_load_saved(existing, WIFI_MAX_SAVED);

    int slot = -1;
    for (int i = 0; i < n; i++) {
        if (existing[i].ssid == ssid) { slot = i; break; }  // already saved -- update in place
    }
    if (slot < 0) {
        if (n < WIFI_MAX_SAVED) {
            slot = n;
            n++;
        } else {
            // Full: drop the oldest (slot 0), shift the rest down one.
            for (int i = 1; i < WIFI_MAX_SAVED; i++) existing[i - 1] = existing[i];
            slot = WIFI_MAX_SAVED - 1;
        }
    }
    existing[slot].ssid = ssid;
    existing[slot].pass = pass;

    wifiPrefs.begin("wifi", false);  // read-write
    wifiPrefs.putInt("count", n);
    for (int i = 0; i < n; i++) {
        wifiPrefs.putString(("ssid" + String(i)).c_str(), existing[i].ssid);
        wifiPrefs.putString(("pass" + String(i)).c_str(), existing[i].pass);
    }
    wifiPrefs.end();
}

static void wifi_forget_all() {
    wifiPrefs.begin("wifi", false);
    wifiPrefs.clear();
    wifiPrefs.end();
    Serial.println("[WiFi] All saved networks forgotten.");
}

// ============================================================================
// Serial line input: blocking with a timeout, resets the timeout on each
// keystroke so a slow typist doesn't get cut off mid-password.
// ============================================================================

static String wifi_read_line(uint32_t timeoutMs) {
    String line = "";
    uint32_t deadline = millis() + timeoutMs;
    while ((int32_t)(deadline - millis()) > 0) {
        if (Serial.available()) {
            char c = Serial.read();
            if (c == '\n' || c == '\r') {
                if (line.length() > 0) return line;
                continue;  // swallow a lone \r or \n before real input starts
            }
            line += c;
            deadline = millis() + timeoutMs;
        }
    }
    return line;  // possibly empty, on timeout
}

// ============================================================================
// Connect + report -- shared by both the "saved network" and "scan" paths.
// ============================================================================

static bool wifi_try_connect(const String& ssid, const String& pass, uint32_t timeoutMs) {
    Serial.printf("[WiFi] Connecting to \"%s\"...\n", ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.length() ? pass.c_str() : NULL);
    uint32_t deadline = millis() + timeoutMs;
    while (WiFi.status() != WL_CONNECTED && (int32_t)(deadline - millis()) > 0) {
        delay(250);
        Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WiFi] Connected. IP: %s\n", WiFi.localIP().toString().c_str());
        return true;
    }
    Serial.println("[WiFi] Failed to connect (wrong password, out of range, or network down).");
    WiFi.disconnect(true);
    return false;
}

// ============================================================================
// Main interactive flow -- entered by sending 'w' at the boot menu. Fully
// synchronous/blocking by design: this only runs on an explicit human
// request, so waiting on Serial input here is exactly what should happen.
// ============================================================================

static void wifi_setup_menu() {
    SavedWifi saved[WIFI_MAX_SAVED];
    int nSaved = wifi_load_saved(saved, WIFI_MAX_SAVED);

    Serial.println("\n=== WiFi setup ===");
    if (nSaved > 0) {
        Serial.println("Saved networks:");
        for (int i = 0; i < nSaved; i++) {
            Serial.printf("  %d) %s\n", i + 1, saved[i].ssid.c_str());
        }
        Serial.println("  f) forget all saved networks");
    }
    Serial.println("  s) scan for networks");
    Serial.println("  (anything else, or 10s of silence) = back to the main menu, no changes");
    Serial.print("> ");

    String choice = wifi_read_line(10000);
    choice.trim();

    if (choice.length() == 0) {
        Serial.println("[WiFi] No input -- skipping WiFi setup.");
        return;
    }
    if (choice == "f" || choice == "F") {
        wifi_forget_all();
        return;
    }
    if (choice == "s" || choice == "S") {
        Serial.println("[WiFi] Scanning...");
        WiFi.mode(WIFI_STA);
        int n = WiFi.scanNetworks();
        if (n <= 0) {
            Serial.println("[WiFi] No networks found.");
            return;
        }
        Serial.println("Networks found:");
        for (int i = 0; i < n && i < 30; i++) {
            Serial.printf("  %2d) %-32s  RSSI %4d  %s\n", i + 1, WiFi.SSID(i).c_str(),
                          (int)WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "open" : "secured");
        }
        Serial.print("Pick a number, or Enter to cancel: ");
        String pick = wifi_read_line(20000);
        pick.trim();
        int idx = pick.toInt();
        if (idx < 1 || idx > n) {
            Serial.println("[WiFi] Cancelled.");
            WiFi.scanDelete();
            return;
        }
        String ssid = WiFi.SSID(idx - 1);
        bool open = WiFi.encryptionType(idx - 1) == WIFI_AUTH_OPEN;
        WiFi.scanDelete();

        String pass = "";
        if (!open) {
            Serial.printf("Password for \"%s\": ", ssid.c_str());
            pass = wifi_read_line(30000);
            pass.trim();
        }
        if (wifi_try_connect(ssid, pass, WIFI_CONNECT_TIMEOUT_MS)) {
            Serial.print("Save this network for next time? [Y/n]: ");
            String save = wifi_read_line(10000);
            save.trim();
            if (save.length() == 0 || save[0] == 'y' || save[0] == 'Y') {
                wifi_save_network(ssid, pass);
                Serial.println("[WiFi] Saved.");
            }
        }
        return;
    }

    int idx = choice.toInt();
    if (idx >= 1 && idx <= nSaved) {
        wifi_try_connect(saved[idx - 1].ssid, saved[idx - 1].pass, WIFI_CONNECT_TIMEOUT_MS);
        return;
    }

    Serial.println("[WiFi] Not a valid choice -- back to the main menu.");
}
