/*
 * capture_spi.h — GPIO-interrupt based capture + frame verification for the
 * clocked (SPI) chips SPIPixels drives: APA102/SK9822/HD107S, WS2801,
 * P9813, SM16716/SM16726.
 *
 * *** SPEED LIMIT: reliably captures up to roughly 200-500 kHz. *** This is
 * a plain GPIO-interrupt bit-bang capture, not a hardware SPI receiver --
 * ESP32 interrupt latency (~1-2us) can't keep up with real-world SPI clocks
 * (WS2801/P9813 default to 1MHz, APA102 to 8MHz, HD107S to 20MHz, SM16716
 * to 16MHz). Frame CONTENT doesn't depend on clock speed, only signal
 * integrity does -- so temporarily lower `spiSpeed` in your SPIPixels
 * output config (e.g. to 200000) while verifying framing with this tool,
 * then restore your normal speed once framing checks out.
 *
 * Wiring: CLK -> SPI_CLK_PIN, DATA(MOSI) -> SPI_DATA_PIN, common GND.
 * *** 3.3V ONLY *** -- level-shift a 5V source before wiring in.
 * SPI mode 0 (CPOL0/CPHA0): data is valid on the clock's RISING edge, which
 * is what every chip in this file uses (see the "Datasheet vs. Silicon"
 * research this tool is built to verify against).
 */
#pragma once
#include <string.h>
#include "spi_decoders.h"

#define SPI_CLK_PIN         5
#define SPI_DATA_PIN        6
#define SPI_IDLE_TIMEOUT_US 5000     // no clock edge for this long = frame done
#define SPI_MAX_BITS        (8192 * 8) // 8192 bytes worth of bits

static volatile uint8_t  spi_bitbuf[SPI_MAX_BITS / 8];
static volatile uint32_t spi_bitcount = 0;
static volatile uint32_t spi_lastEdgeUs = 0;
static volatile bool     spi_capturing = false;

// ---- ISR: fires on every CLK rising edge, shifts one DATA bit into spi_bitbuf ----
static void IRAM_ATTR spi_on_clk_rising() {
    if (spi_bitcount < SPI_MAX_BITS) {
        int bit = digitalRead(SPI_DATA_PIN);
        uint32_t byteIdx = spi_bitcount / 8;
        int bitInByte = 7 - (int)(spi_bitcount % 8);
        if (bitInByte == 7) spi_bitbuf[byteIdx] = 0; // fresh byte
        if (bit) spi_bitbuf[byteIdx] |= (1 << bitInByte);
        spi_bitcount = spi_bitcount + 1; // ++ on a volatile is deprecated (C++20)
    }
    spi_lastEdgeUs = micros();
    spi_capturing = true;
}

// ---- Mode lifecycle: called from the .ino's menu handler ----
static void spi_capture_begin() {
    Serial.printf("\n[SPI] CLK=GPIO%d DATA=GPIO%d. Idle %dus ends a frame.\n",
                  SPI_CLK_PIN, SPI_DATA_PIN, SPI_IDLE_TIMEOUT_US);
    Serial.println("[SPI] Reliable to ~200-500kHz -- lower spiSpeed in your FPP config while testing.");
    Serial.println("[SPI] Feed CLK+DATA in. Waiting for frames (any key returns to menu)...\n");
    pinMode(SPI_CLK_PIN, INPUT);
    pinMode(SPI_DATA_PIN, INPUT);
    spi_bitcount = 0;
    spi_capturing = false;
    attachInterrupt(digitalPinToInterrupt(SPI_CLK_PIN), spi_on_clk_rising, RISING);
}

static void spi_capture_end() {
    detachInterrupt(digitalPinToInterrupt(SPI_CLK_PIN));
}

// Runs once per idle-detected frame. Returns immediately if nothing's ready.
static void spi_capture_poll() {
    if (!spi_capturing) return;
    if (micros() - spi_lastEdgeUs < SPI_IDLE_TIMEOUT_US) return; // still receiving

    detachInterrupt(digitalPinToInterrupt(SPI_CLK_PIN));
    uint32_t nBits = spi_bitcount;
    if (nBits < 8) {
        spi_bitcount = 0; spi_capturing = false;
        attachInterrupt(digitalPinToInterrupt(SPI_CLK_PIN), spi_on_clk_rising, RISING);
        return;
    }
    static uint8_t snap[SPI_MAX_BITS / 8];
    memcpy(snap, (const void*)spi_bitbuf, (nBits + 7) / 8);
    int nBytes = nBits / 8;

    Serial.println("---- SPI frame ----");
    Serial.printf("bits     : %u  (%d whole bytes)\n", (unsigned)nBits, nBytes);
    Serial.print("bytes    :");
    for (int i = 0; i < nBytes && i < 64; i++) Serial.printf(" %02X", snap[i]);
    if (nBytes > 64) Serial.print(" ...");
    Serial.println();
    Serial.println("checked against every known frame shape (only the real chip's will look sane):");
    spi_check_apa102(snap, nBytes, "APA102/SK9822/HD107S", Serial);
    spi_check_p9813(snap, nBytes, Serial);
    spi_check_ws2801(snap, nBytes, Serial);
    spi_check_sm16716(snap, nBits, Serial);
    Serial.println();

    spi_bitcount = 0;
    spi_capturing = false;
    attachInterrupt(digitalPinToInterrupt(SPI_CLK_PIN), spi_on_clk_rising, RISING);
}
