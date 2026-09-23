// The bus (SPEC.md §2, §4, §14's /INT, §15's RST; PLAN.md section 3).
//
// Two PIO programs on pio1 watch the pins (bus.pio). tmsWrite is pico9918's
// and hands every write to core 1: port from MODE1:MODE, byte from CD0-7.
// tmsRead answers every read at once, from a word staged ahead of it that holds
// what each of the four ports reads now (vdp_staged), and then tells core 1
// which port it answered and with what. Core 1 applies the access to the card
// (vdp_write, vdp_read_served) in one interrupt at the latch's priority, so the
// two never preempt each other, and restages.
//
// Restaging is bus_sync: whatever changes what a port would read — an access,
// a latch, a row's publication, a reset, the debug link's own writes — calls it
// on core 1 with interrupts held off or from an interrupt at the bus's priority,
// and it also drives /INT on GPIO 22. RST on GPIO 23 performs §15.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "vdp.h"

// Core 1, once, before anything that calls bus_sync: pins, programs and the
// interrupts, on core 1's NVIC. The card is the renderer's. Only on the PRO: on
// a Pico 2 the bus is not started and bus_sync does nothing.
void bus_start(vdp_t *card);

// Core 1, interrupts off or at the bus's priority: stage what each port reads
// now, and drive /INT to match the card.
void bus_sync(void);

// Core 1, from the latch interrupt: note what the PIO's FIFOs have seen since.
void bus_line(void);

#if PICOVDP_DEBUG

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
} bus_stats_t;

// Any core: the counts, reset once read if asked.
void bus_stats(bus_stats_t *out, bool reset);

#endif
