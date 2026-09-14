/*
 * Project: pico9918 - vga
 *
 * Copyright (c) 2024 Troy Schrapel
 *
 * This code is licensed under the MIT license
 *
 * https://github.com/visrealm/pico9918
 *
 */

// 6502-PICOVDP (PLAN.md Appendix A). Changed from pico9918's src/vga/vga.c:
//
// - VGA 640 x 480 at 60 Hz only. vga-modes.c's table, the interlaced and SCART
//   paths, pixel scaling options and the scanline effect are gone.
// - The sync program raises PIO interrupt 0 at the back porch before every
//   screen line's first VGA line, and interrupt 1 instead before screen line
//   0, so every one of §3's 262 line starts reaches the firmware on time, the
//   blanking lines' included. pico9918 asked core 1 for rows through the
//   inter-core FIFO from the DMA interrupt, whose FIFOs run lines ahead.
// - The RGB buffer each row sends is chosen by the firmware at its line start,
//   for both VGA lines, rather than alternating between two buffers. That is
//   what lets a late row show the last completed one (§18).
// - The sync buffers are one table of 525 lines, built once.
// - Interrupt priorities are set explicitly: both VGA interrupts are the
//   highest on core 0 (PLAN.md section 3).
//
// The PIO programs (vga.pio) and the timing arithmetic are pico9918's.

#include "vga.h"

#include "vga.pio.h"
#include "pio_utils.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "pico/binary_info.h"

#define VGA_PIO pio0
#define SYNC_SM 0
#define RGB_SM 1

// PIO interrupt flags the sync program raises: a screen line begins, and the
// frame's first does. The RGB program waits on vga_rgb_RGB_IRQ (4).
#define LINE_FLAG 0
#define FRAME_FLAG 1

// VGA 640 x 480 at 60 Hz (pico9918's vga-modes.c, from tinyvga.com).
#define PIXEL_CLOCK_KHZ 25175
#define H_DISPLAY 640
#define H_FRONT_PORCH 16
#define H_SYNC 96
#define H_BACK_PORCH 48
#define V_DISPLAY VGA_ACTIVE_LINES
#define V_FRONT_PORCH 10
#define V_SYNC 2
#define V_BACK_PORCH 33

// §3: 262 screen lines of two VGA lines, and the odd one.
#define SCREEN_LINES 262
#define PICTURE_ROWS 240

bi_decl(bi_1pin_with_name(VGA_SYNC_PINS_START, "H Sync"));
bi_decl(bi_1pin_with_name(VGA_SYNC_PINS_START + 1, "V Sync"));
bi_decl(bi_pin_mask_with_names(0xf << VGA_RGB_PINS_START, "Red (LSB - MSB)"));
bi_decl(bi_pin_mask_with_names(0xf << (VGA_RGB_PINS_START + 4), "Green (LSB - MSB)"));
bi_decl(bi_pin_mask_with_names(0xf << (VGA_RGB_PINS_START + 8), "Blue (LSB - MSB)"));

// Four words a VGA line, indexed as pico9918's counter runs: lines 0-1 vertical
// sync, 2-34 back porch, 35-514 the picture, 515-524 front porch.
static uint32_t sync_words[VGA_LINES][4];
static const uint32_t black[VGA_RGB_WORDS];

static vga_line_start_fn line_start_fn;
static int sync_channel, rgb_channel;
static uint32_t pio_divider;
static unsigned rgb_program_offset;

// Written by the interrupts only.
static uint16_t vga_line;                 // the VGA line the sync DMA last queued
static uint16_t screen_line = 0xffff;     // 0xffff until the first frame starts
static const uint32_t *repeat;            // the buffer a row's second VGA line sends again
static uint32_t slips;

// avoid bringing in math.h
static int roundflt(float x) {
    return x < 0.0f ? (int)(x - 0.5f) : (int)(x + 0.5f);
}

static void build_sync_words(void) {
    uint32_t sys_khz = clock_get_hz(clk_sys) / 1000;
    uint32_t min_khz = PIXEL_CLOCK_KHZ * vga_rgb_LOOP_TICKS;
    if (min_khz < 50000) min_khz *= 2;
    pio_divider = (uint32_t)roundflt(sys_khz / (float)min_khz);
    float pio_khz = sys_khz / (float)pio_divider;
    float per_pixel = pio_khz / (float)PIXEL_CLOCK_KHZ;

    const uint32_t active = (uint32_t)roundflt(per_pixel * H_DISPLAY) - vga_sync_SETUP_OVERHEAD;
    const uint32_t front = (uint32_t)roundflt(per_pixel * H_FRONT_PORCH) - vga_sync_SETUP_OVERHEAD;
    const uint32_t sync = (uint32_t)roundflt(per_pixel * H_SYNC) - vga_sync_SETUP_OVERHEAD;
    const uint32_t back = (uint32_t)roundflt(per_pixel * H_BACK_PORCH) - vga_sync_SETUP_OVERHEAD;

    // Both syncs are active low.
    const uint32_t h_off = 1u << vga_sync_WORD_HSYNC_OFFSET, v_off = 1u << vga_sync_WORD_VSYNC_OFFSET;
    const uint32_t nop = (uint32_t)pio_encode_nop() << vga_sync_WORD_EXEC_OFFSET;
    const uint32_t picture = (uint32_t)pio_encode_irq_set(false, vga_rgb_RGB_IRQ) << vga_sync_WORD_EXEC_OFFSET;
    const uint32_t line_flag = (uint32_t)pio_encode_irq_set(false, LINE_FLAG) << vga_sync_WORD_EXEC_OFFSET;
    const uint32_t frame_flag = (uint32_t)pio_encode_irq_set(false, FRAME_FLAG) << vga_sync_WORD_EXEC_OFFSET;

    const unsigned first_picture = V_SYNC + V_BACK_PORCH;
    for (unsigned line = 0; line < VGA_LINES; line++) {
        const bool vsync = line < V_SYNC;
        const bool active_line = line >= first_picture && line < first_picture + V_DISPLAY;
        const uint32_t v = vsync ? 0 : v_off;

        // a: the line counted from the picture's first. The back porch of VGA
        // line a is followed by the picture of a + 1: screen line s begins at
        // the back porch of VGA line 2s - 1, and screen line 0 at the odd line's.
        const unsigned a = (line + VGA_LINES - first_picture) % VGA_LINES;
        uint32_t flag = nop;
        if (a == VGA_LINES - 1) flag = frame_flag;
        else if ((a & 1) && a <= 2 * SCREEN_LINES - 3) flag = line_flag;

        sync_words[line][0] = (active_line ? picture : nop) | h_off | v | active;
        sync_words[line][1] = nop | h_off | v | front;
        sync_words[line][2] = nop | v | sync;
        sync_words[line][3] = flag | h_off | v | back;
    }
}

// A screen line begins: which one, and the buffer its row sends.
static void __isr __time_critical_func(line_start_isr)(void) {
    const uint32_t flags = VGA_PIO->irq & ((1u << LINE_FLAG) | (1u << FRAME_FLAG));
    VGA_PIO->irq = flags;

    uint16_t line;
    if (flags & (1u << FRAME_FLAG)) {
        if (screen_line != SCREEN_LINES - 1 && screen_line != 0xffff) slips++;
        line = 0;
    } else if (screen_line == 0xffff) {
        return;  // not yet at a frame's start
    } else if ((line = (uint16_t)(screen_line + 1)) >= SCREEN_LINES) {
        slips++;
        line = 0;
    }
    screen_line = line;

    const uint32_t *buffer = line_start_fn(line);
    if (line < PICTURE_ROWS) {
        repeat = buffer;
        dma_channel_set_read_addr(rgb_channel, buffer, true);
    }
}

// The sync program's next line; a row's second VGA line.
static void __isr __time_critical_func(dma_isr)(void) {
    const uint32_t sync_mask = 1u << sync_channel, rgb_mask = 1u << rgb_channel;
    if (dma_hw->ints0 & sync_mask) {
        dma_hw->ints0 = sync_mask;
        if (++vga_line >= VGA_LINES) vga_line = 0;
        dma_channel_set_read_addr(sync_channel, sync_words[vga_line], true);
    }
    if (dma_hw->ints0 & rgb_mask) {
        dma_hw->ints0 = rgb_mask;
        if (repeat) {
            dma_channel_set_read_addr(rgb_channel, repeat, true);
            repeat = NULL;
        }
    }
}

static void init_sync(void) {
    build_sync_words();
    for (unsigned i = 0; i < VGA_SYNC_PINS_COUNT; i++) pio_gpio_init(VGA_PIO, VGA_SYNC_PINS_START + i);

    unsigned offset = pio_add_program(VGA_PIO, &vga_sync_program);
    pio_sm_set_consecutive_pindirs(VGA_PIO, SYNC_SM, VGA_SYNC_PINS_START, VGA_SYNC_PINS_COUNT, true);
    pio_sm_config config = vga_sync_program_get_default_config(offset);
    sm_config_set_out_pins(&config, VGA_SYNC_PINS_START, VGA_SYNC_PINS_COUNT);
    sm_config_set_clkdiv(&config, (float)pio_divider);
    sm_config_set_out_shift(&config, true, true, 32);  // R shift, autopull @ 32 bits
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);
    pio_sm_init(VGA_PIO, SYNC_SM, offset, &config);

    sync_channel = dma_claim_unused_channel(true);
    dma_channel_config dma = dma_channel_get_default_config(sync_channel);
    channel_config_set_transfer_data_size(&dma, DMA_SIZE_32);
    channel_config_set_read_increment(&dma, true);
    channel_config_set_write_increment(&dma, false);
    channel_config_set_dreq(&dma, pio_get_dreq(VGA_PIO, SYNC_SM, true));
    dma_channel_configure(sync_channel, &dma, &VGA_PIO->txf[SYNC_SM], sync_words[0], 4, false);
    dma_channel_set_irq0_enabled(sync_channel, true);
}

static void init_rgb(void) {
    const uint32_t sys_khz = clock_get_hz(clk_sys) / 1000;
    const float per_pixel = (sys_khz / (float)pio_divider) / (float)PIXEL_CLOCK_KHZ;
    const uint32_t cycles_per_pixel = (uint32_t)roundflt(per_pixel);

    // The RGB program with its per-pixel delay patched in.
    uint16_t instructions[vga_rgb_program.length];
    for (int i = 0; i < vga_rgb_program.length; i++) instructions[i] = vga_rgb_program.instructions[i];
    instructions[vga_rgb_DELAY_INSTR] |= (uint16_t)pio_encode_delay(cycles_per_pixel - vga_rgb_LOOP_TICKS);
    const pio_program_t program = {
        .instructions = instructions,
        .length = vga_rgb_program.length,
        .origin = vga_rgb_program.origin,
    };

    for (unsigned i = 0; i < VGA_RGB_PINS_COUNT; i++) pio_gpio_init(VGA_PIO, VGA_RGB_PINS_START + i);
    pio_sm_set_consecutive_pindirs(VGA_PIO, RGB_SM, VGA_RGB_PINS_START, VGA_RGB_PINS_COUNT, true);
    // 640 pixels and the guard word's two: y is one less.
    pio_set_y(VGA_PIO, RGB_SM, 2 * VGA_RGB_WORDS - 1);

    rgb_program_offset = pio_add_program(VGA_PIO, &program);
    pio_sm_config config = vga_rgb_program_get_default_config(rgb_program_offset);
    sm_config_set_out_pins(&config, VGA_RGB_PINS_START, VGA_RGB_PINS_COUNT);
    sm_config_set_clkdiv(&config, (float)pio_divider);
    sm_config_set_out_shift(&config, true, true, 32);
    pio_sm_init(VGA_PIO, RGB_SM, rgb_program_offset, &config);

    rgb_channel = dma_claim_unused_channel(true);
    dma_channel_config dma = dma_channel_get_default_config(rgb_channel);
    channel_config_set_transfer_data_size(&dma, DMA_SIZE_32);
    channel_config_set_read_increment(&dma, true);
    channel_config_set_write_increment(&dma, false);
    channel_config_set_dreq(&dma, pio_get_dreq(VGA_PIO, RGB_SM, true));
    dma_channel_configure(rgb_channel, &dma, &VGA_PIO->txf[RGB_SM], black, VGA_RGB_WORDS, false);
    dma_channel_set_irq0_enabled(rgb_channel, true);
}

void vga_init(vga_line_start_fn line_start) {
    line_start_fn = line_start;
    init_sync();
    init_rgb();

    irq_set_exclusive_handler(DMA_IRQ_0, dma_isr);
    irq_set_priority(DMA_IRQ_0, PICO_HIGHEST_IRQ_PRIORITY);
    irq_set_exclusive_handler(PIO0_IRQ_0, line_start_isr);
    irq_set_priority(PIO0_IRQ_0, PICO_HIGHEST_IRQ_PRIORITY);
    pio_set_irq0_source_mask_enabled(VGA_PIO, (1u << pis_interrupt0) | (1u << pis_interrupt1), true);
}

void vga_start(void) {
    pio_interrupt_clear(VGA_PIO, LINE_FLAG);
    pio_interrupt_clear(VGA_PIO, FRAME_FLAG);
    irq_set_enabled(DMA_IRQ_0, true);
    irq_set_enabled(PIO0_IRQ_0, true);
    dma_channel_start(sync_channel);
    pio_sm_set_enabled(VGA_PIO, SYNC_SM, true);
    pio_sm_set_enabled(VGA_PIO, RGB_SM, true);
}

uint32_t vga_pio_divider(void) {
    return pio_divider;
}

uint32_t vga_display_line_cycles(void) {
    const uint32_t sys_khz = clock_get_hz(clk_sys) / 1000;
    const float per_pixel = (sys_khz / (float)pio_divider) / (float)PIXEL_CLOCK_KHZ;
    const uint32_t ticks = (uint32_t)(roundflt(per_pixel * H_DISPLAY) + roundflt(per_pixel * H_FRONT_PORCH) +
                                      roundflt(per_pixel * H_SYNC) + roundflt(per_pixel * H_BACK_PORCH));
    return 2 * ticks * pio_divider;
}

uint32_t vga_raster_slips(void) {
    return slips;
}
