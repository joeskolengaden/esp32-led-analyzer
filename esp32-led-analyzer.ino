/*
 * ESP32-S3 LED Signal Analyzer
 * ----------------------------
 * Protocol-aware capture for the fpp-bbb-pixels / SPIPixels chip set:
 *
 *  [1] Single-wire (RMT RX) -- WS281x family, TM1814/TM1829/TM1914
 *      (inverted), UCS7604, UCS8903/8904 (16-bit), WS2805, SM16825E.
 *      Reports polarity, T0/T1/period, timing-profile classification
 *      against this session's library-consensus tolerance windows (see
 *      protocols.h), and scans for known preamble/trailer byte signatures
 *      (TM1814/TM1914/UCS7604 preambles, the SM16825E current-gain trailer).
 *
 *  [2] SPI/clocked (GPIO-ISR) -- APA102/SK9822/HD107S, WS2801, P9813,
 *      SM16716/SM16726. Reports the raw byte/bit stream and checks it
 *      against every known frame shape (only the real chip's will look
 *      sane). Reliable to ~200-500kHz -- see capture_spi.h for why, and
 *      lower `spiSpeed` in your SPIPixels config while testing.
 *
 * This exists to close the loop this session's work flagged as open: the
 * TM1814/TM1829/TM1914 inverted waveforms have never been watched on a
 * scope, and the SPI framing fixes (P9813 end-length, SM16716) have never
 * been checked against a real capture either.
 *
 * Board: ESP32-S3 (Arduino-ESP32 core 3.0.0+). Serial: 115200 baud.
 * Wiring: see capture_singlewire.h / capture_spi.h for pin numbers.
 * *** 3.3V ONLY on every input pin *** -- level-shift any 5V source first
 * (e.g. 1k from source to pin, 2k from pin to GND).
 */
#include "capture_singlewire.h"
#include "capture_spi.h"

enum Mode { MODE_MENU, MODE_SINGLEWIRE, MODE_SPI };
static Mode mode = MODE_MENU;

static void print_menu() {
    Serial.println("\n=== ESP32-S3 LED Signal Analyzer ===");
    Serial.println("!! GPIO is 3.3V ONLY -- a bare 5V data/clock line WILL damage this board.");
    Serial.println("!! No level-shifter chip needed: a 1k resistor (source->pin) + a 2k resistor");
    Serial.println("!! (pin->GND) does the same job for a few cents. Don't feed 5V in unshifted.");
    Serial.println("  1) Single-wire capture (WS281x family, TM1814/1829/1914, UCS7604, WS2805, SM16825E...)");
    Serial.println("  2) SPI capture (APA102/SK9822/HD107S, WS2801, P9813, SM16716/SM16726)");
    Serial.println("Send 1 or 2 to start. While capturing, send any key to return here.\n");
}

void setup() {
    Serial.begin(115200);
    delay(300);
    print_menu();
}

void loop() {
    if (mode == MODE_MENU) {
        if (Serial.available()) {
            char c = Serial.read();
            if (c == '1') { mode = MODE_SINGLEWIRE; sw_capture_begin(); }
            else if (c == '2') { mode = MODE_SPI; spi_capture_begin(); }
        }
        return;
    }

    // Any key while capturing returns to the menu.
    if (Serial.available()) {
        Serial.read();
        if (mode == MODE_SINGLEWIRE) sw_capture_end();
        else if (mode == MODE_SPI) spi_capture_end();
        mode = MODE_MENU;
        print_menu();
        return;
    }

    if (mode == MODE_SINGLEWIRE) sw_capture_poll();
    else if (mode == MODE_SPI) spi_capture_poll();
}
