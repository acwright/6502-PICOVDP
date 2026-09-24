// The bus (SPEC.md §2, §4, §14's /INT, §15's RST; PLAN.md section 3).
//
// Two PIO programs on pio1 watch the pins (bus.pio). tmsWrite is pico9918's
// and hands every write to core 1: port from MODE1:MODE, byte from CD0-7.
// tmsRead answers every read at once, from a word staged ahead of it that holds
// what each of the four ports reads now (vdp_staged), and then tells core 1
// which port it answered and with what. Core 1 applies the access to the card
// (vdp_write, vdp_read_served) in its highest-priority interrupt, and restages.
//
// Phase 13: nothing on core 1 holds the bus interrupt off for long. It is above
// the latch, which masks it only for its moment (vdp_latch_take); the renderer
// masks it for each 64-byte step of a FONT copy and for a row's publication, a
// hundred cycles or so each. So an access's restage is done well inside the 2 us a 2 MHz 6502
// leaves before its next access (docs/results/phase-13.md).
//
// Restaging — staging the word, and driving /INT on GPIO 22 to match the card
// — happens in the bus interrupt only, so the staging FIFO has one writer.
// Whatever else changes what a port would read — a latch, a row's publication,
// a reset, the debug link's own writes — asks for it with bus_restage. RST on
// GPIO 23 performs §15.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "vdp.h"

// Core 1, once, before anything that calls bus_restage: pins, programs and the
// interrupts, on core 1's NVIC. The card is the renderer's. Only on the PRO: on
// a Pico 2 the bus is not started and bus_restage does nothing.
void bus_start(vdp_t *card);

// Core 1, anywhere, after the card has changed: the bus interrupt restages as
// soon as nothing masks it — at once from the latch or the thread.
void bus_restage(void);

// Core 1, from the latch interrupt once its own part is done: note what the
// PIO's FIFOs have seen since the last, and restage. `line_start` is the
// shared timer's count as core 0 began the line (debug builds' status lag).
void bus_line(uint32_t line_start);

#if PICOVDP_DEBUG

#define BUS_LAG_BINS 64
#define BUS_STALE_KEPT 4
#define BUS_LAG_BIN 64

typedef struct bus_stats {
    uint32_t writes;            // accesses taken
    uint32_t reads;
    uint32_t stale_data;        // data reads served a byte the card had moved past before its restage landed
    uint32_t stale_status;      // status reads likewise: a latch had moved the raster or a flag since (§6)
    uint32_t coincident;        // a read and a write both waiting at once: their order is lost
    uint32_t write_overruns;    // lines on which tmsWrite stalled on a full FIFO: writes missed
    uint32_t read_overruns;     // lines on which tmsRead stalled on a full FIFO: reads unanswered
    uint32_t staging_waits;     // restages that found the staging FIFO full and waited
    uint32_t resets;            // RST's falling edges
    uint32_t isr_max;           // cycles, the bus interrupt
    uint32_t int_level;         // /INT as driven now: 1 asserted
    // Phase 13: how long after a line began its status was staged and /INT
    // driven to match — core 0's line start to the bus interrupt's restage for
    // that latch, on the shared timer (cycles at clk_sys).
    uint32_t isr_mean;          // cycles, the bus interrupt, over every run of it
    uint32_t lag_count;
    uint32_t lag_max;
    uint32_t lag_mean;
    uint16_t lag_histogram[BUS_LAG_BINS];   // BUS_LAG_BIN cycles a bin, saturating; the last holds the rest
    // The last BUS_STALE_KEPT stale data reads, for their story: the port, the
    // byte served and the prefetch the card held, the word last staged, and
    // the cycles from that staging and from the interrupt's entry to the read.
    struct {
        uint8_t port, served, held;
        uint32_t staged, since_stage, since_entry;
    } stale[BUS_STALE_KEPT];
} bus_stats_t;

// Any core: the counts, reset once read if asked.
void bus_stats(bus_stats_t *out, bool reset);

#endif
