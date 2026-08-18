/*
 * capture_singlewire.h — RMT-RX based capture + protocol-aware decode for
 * single-wire chips (WS281x family, TM1814/1829/1914, UCS7604/8903/8904,
 * WS2805, SM16825E, etc).
 *
 * Board: ESP32-S3 (needs the IDF5 RMT RX driver bundled in Arduino-ESP32
 * core 3.0.0+). *** 3.3V ONLY on the data pin *** -- level-shift a 5V
 * source (e.g. 1k source->pin, 2k pin->GND divider) before wiring in.
 */
#pragma once
#include "driver/rmt_rx.h"
#include "protocols.h"

#define SW_DATA_PIN        4          // GPIO the (level-shifted) data line feeds into
#define SW_RMT_RES_HZ      80000000   // 80 MHz -> 12.5 ns per tick
#define SW_MAX_SYMBOLS     4096       // each symbol = 1 bit; 4096 bits = 512 bytes
#define SW_RESET_GAP_NS    30000      // idle longer than this ends a frame
#define SW_MIN_PULSE_NS    80         // ignore glitches shorter than this
#define SW_TICK_NS         (1000000000.0 / SW_RMT_RES_HZ)

static rmt_channel_handle_t sw_rx_chan = NULL;
static rmt_symbol_word_t    sw_symbols[SW_MAX_SYMBOLS];
static volatile size_t      sw_got_symbols = 0;
static volatile bool        sw_frame_ready = false;
static rmt_receive_config_t sw_rx_cfg;

static bool IRAM_ATTR sw_on_recv_done(rmt_channel_handle_t ch,
                                       const rmt_rx_done_event_data_t* ed, void* u) {
    if (!sw_frame_ready) {                 // don't clobber an unprocessed frame
        sw_got_symbols = ed->num_symbols;
        sw_frame_ready = true;
    }
    return false;
}

static void sw_arm() {
    rmt_receive(sw_rx_chan, sw_symbols, sizeof(sw_symbols), &sw_rx_cfg);
}

static void sw_capture_begin() {
    Serial.printf("\n[single-wire] GPIO%d, %.1f ns/tick, idle-gap %dns ends a frame.\n",
                  SW_DATA_PIN, SW_TICK_NS, SW_RESET_GAP_NS);
    Serial.println("[single-wire] Feed a data line in. Waiting for frames (any key returns to menu)...\n");

    rmt_rx_channel_config_t cc = {};
    cc.gpio_num          = (gpio_num_t)SW_DATA_PIN;
    cc.clk_src           = RMT_CLK_SRC_DEFAULT;
    cc.resolution_hz     = SW_RMT_RES_HZ;
    cc.mem_block_symbols = SW_MAX_SYMBOLS;
    cc.flags.with_dma    = true;           // S3 supports RMT DMA (needed for big buffers)
    if (rmt_new_rx_channel(&cc, &sw_rx_chan) != ESP_OK) {
        cc.flags.with_dma = false;         // fall back if DMA unavailable
        cc.mem_block_symbols = 256;
        ESP_ERROR_CHECK(rmt_new_rx_channel(&cc, &sw_rx_chan));
        Serial.println("[single-wire] (no DMA: capped at 256 bits/frame)");
    }
    rmt_rx_event_callbacks_t cbs = { .on_recv_done = sw_on_recv_done };
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(sw_rx_chan, &cbs, NULL));
    ESP_ERROR_CHECK(rmt_enable(sw_rx_chan));

    sw_rx_cfg.signal_range_min_ns = SW_MIN_PULSE_NS;
    sw_rx_cfg.signal_range_max_ns = SW_RESET_GAP_NS;
    sw_frame_ready = false;
    sw_arm();
}

static void sw_capture_end() {
    if (sw_rx_chan) {
        rmt_disable(sw_rx_chan);
        rmt_del_channel(sw_rx_chan);
        sw_rx_chan = NULL;
    }
}

static inline int sw_dur_ns(uint16_t ticks) { return (int)(ticks * SW_TICK_NS + 0.5); }

// Runs once per captured frame. Returns immediately if nothing's ready yet.
static void sw_capture_poll() {
    if (!sw_frame_ready) return;
    size_t n = sw_got_symbols;
    if (n < 8) { sw_frame_ready = false; sw_arm(); return; }  // noise

    // Gather high-phase and low-phase durations across all symbols.
    long hiSum = 0, loSum = 0; int hiN = 0, loN = 0;
    int hiMin = 1 << 30, hiMax = 0, loMin = 1 << 30, loMax = 0;
    for (size_t i = 0; i < n; i++) {
        rmt_symbol_word_t s = sw_symbols[i];
        int d0 = sw_dur_ns(s.duration0), d1 = sw_dur_ns(s.duration1);
        if (s.duration0) {
            if (s.level0) { hiSum += d0; hiN++; if (d0 < hiMin) hiMin = d0; if (d0 > hiMax) hiMax = d0; }
            else          { loSum += d0; loN++; if (d0 < loMin) loMin = d0; if (d0 > loMax) loMax = d0; }
        }
        if (s.duration1) {
            if (s.level1) { hiSum += d1; hiN++; if (d1 < hiMin) hiMin = d1; if (d1 > hiMax) hiMax = d1; }
            else          { loSum += d1; loN++; if (d1 < loMin) loMin = d1; if (d1 > loMax) loMax = d1; }
        }
    }

    // The bit-encoding phase is the one with the wider spread (bimodal: T0/T1).
    // Non-inverted chips encode in the HIGH phase; inverted chips (idle-high,
    // TM1814/TM1829/TM1914) encode in the LOW phase.
    bool inverted = (loMax - loMin) > (hiMax - hiMin);
    int aMin = inverted ? loMin : hiMin;
    int aMax = inverted ? loMax : hiMax;
    int thresh = (aMin + aMax) / 2;

    long t0sum = 0, t1sum = 0; int t0n = 0, t1n = 0;
    static uint8_t bytes[SW_MAX_SYMBOLS / 8];
    int nbits = 0;
    uint8_t cur = 0;
    for (size_t i = 0; i < n; i++) {
        rmt_symbol_word_t s = sw_symbols[i];
        int act = -1;
        if (inverted) {
            if (!s.level0 && s.duration0) act = sw_dur_ns(s.duration0);
            else if (!s.level1 && s.duration1) act = sw_dur_ns(s.duration1);
        } else {
            if (s.level0 && s.duration0) act = sw_dur_ns(s.duration0);
            else if (s.level1 && s.duration1) act = sw_dur_ns(s.duration1);
        }
        if (act < 0) continue;
        int bit = (act > thresh) ? 1 : 0;
        if (bit) { t1sum += act; t1n++; } else { t0sum += act; t0n++; }
        cur = (cur << 1) | bit;
        if ((++nbits % 8) == 0) bytes[nbits / 8 - 1] = cur, cur = 0;
        if (nbits / 8 >= (int)sizeof(bytes)) break;
    }
    int nBytes = nbits / 8;

    int periodAvg = (hiN + loN) ? (int)((hiSum + loSum) / ((hiN + loN) / 2 ? (hiN + loN) / 2 : 1)) : 0;
    int t0Avg = t0n ? (int)(t0sum / t0n) : 0;
    int t1Avg = t1n ? (int)(t1sum / t1n) : 0;

    Serial.println("---- single-wire frame ----");
    Serial.printf("polarity : %s\n", inverted ? "INVERTED (idle HIGH) -- TM1814/TM1829/TM1914-style"
                                               : "normal (idle low) -- WS281x-style");
    Serial.printf("bits     : %d  (%d bytes)\n", nbits, nBytes);
    Serial.printf("T0%c      : ~%dns (measured range %d-%dns)\n", inverted ? 'L' : 'H', t0Avg, aMin, thresh);
    Serial.printf("T1%c      : ~%dns (measured range %d-%dns)\n", inverted ? 'L' : 'H', t1Avg, thresh, aMax);
    Serial.printf("period   : ~%dns (~%dkHz)\n", periodAvg, periodAvg ? (int)(1000000 / periodAvg) : 0);
    Serial.println("timing classification:");
    classify_timing(inverted, t0Avg, t1Avg, periodAvg, Serial);
    Serial.println("preamble/trailer signatures:");
    match_signatures(bytes, nBytes, Serial);
    Serial.print("bytes    :");
    for (int i = 0; i < nBytes && i < 64; i++) Serial.printf(" %02X", bytes[i]);
    if (nBytes > 64) Serial.print(" ...");
    Serial.println("\n");

    sw_frame_ready = false;
    sw_arm();
}
