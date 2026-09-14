/*
 * Project: pico9918 - vga
 *
 * Copyright (c) 2024 Troy Schrapel
 *
 * This code is licensed under the MIT license
 *
 * https://github.com/visrealm/pico-56
 *
 */

// 6502-PICOVDP: the driver cut down to VGA 640 x 480, with SPEC §3's raster in
// place of pico9918's line requests. The sync program marks the start of every
// screen line — 262 of them, two VGA lines each, and the odd VGA line after
// the last — with a PIO interrupt, and the driver calls back into the firmware
// there. The RGB buffer a picture row sends is chosen at that moment, for both
// of its VGA lines (§18's late lines). See vga.c for what changed.

#pragma once

#include <stdint.h>

#define VGA_SYNC_PINS_START 0
#define VGA_SYNC_PINS_COUNT 2
#define VGA_RGB_PINS_START 2
#define VGA_RGB_PINS_COUNT 12

// §3: 525 VGA lines a frame, 480 of them the picture.
#define VGA_LINES 525
#define VGA_ACTIVE_LINES 480

// An RGB buffer: 640 pixels of 12-bit 0x0BGR, two to a word, low pixel first,
// then a guard word of zeros for the PIO's autopull (pico9918's).
#define VGA_RGB_WORDS 321

// A screen line begins, 0-261 (§3): the start of the back porch before its
// first VGA line's picture. Called from the line-start interrupt on core 0,
// which nothing on that core preempts. For a picture row, screen_line < 240,
// returns the buffer that row sends, for both of its VGA lines; it must not
// change until the next call. Otherwise the return value is ignored.
typedef const uint32_t *(*vga_line_start_fn)(uint16_t screen_line);

// Set up the sync and RGB state machines, their DMA and the interrupts, at the
// system clock as it now is. Nothing runs until vga_start.
void vga_init(vga_line_start_fn line_start);
void vga_start(void);

// The PIO clock divider from the system clock: 7 at 352 MHz, so one display
// line, 2 x 1,598 PIO ticks, is 22,372 system clock cycles.
uint32_t vga_pio_divider(void);
uint32_t vga_display_line_cycles(void);

// Line starts whose predecessor was not the line before it: the raster slipped
// because the line-start interrupt was held off for more than a line.
uint32_t vga_raster_slips(void);
