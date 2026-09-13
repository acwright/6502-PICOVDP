// Phase 1 timing spike, RP2350 side (PLAN.md section 6, Phase 1).
//
// Core 1 builds every scene's lines and times each stage with the M33's DWT
// cycle counter, interrupts off. Core 0 owns USB and the clock: send 'r' over
// the CDC port and it runs the suite at 302.4 MHz and then 352 MHz, printing
// one machine-readable line per result (spike/report.mjs turns them into
// tables), and ends with "END".
//
// Send 's' instead for the split suite: sprites on core 0 (spike_sprites_split),
// with the interrupts both cores will really take running for real — the bus on
// core 1 every 2 µs, VGA on core 0 every VGA line — and each line timed from its
// latch to its last expanded pixel.

#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/pwm.h"
#include "hardware/structs/m33.h"
#include "hardware/sync.h"
#include "hardware/vreg.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "render.h"
#include "scenes.h"

#define FRAMES 20

typedef struct {
    uint32_t pll, div1, div2;
    enum vreg_voltage voltage;
} clock_preset_t;

// pico9918's VGA presets 1 and 2 (§2). Never preset 0, 252 MHz.
static const clock_preset_t presets[] = {
    { 1512000000, 5, 1, VREG_VOLTAGE_1_20 },   // 302.4 MHz
    { 1056000000, 3, 1, VREG_VOLTAGE_1_30 },   // 352 MHz
};

enum { ST_EVAL, ST_L0, ST_L1, ST_SPR, ST_EXP, ST_TOTAL, ST_COUNT };
static const char *const stage_names[ST_COUNT] = { "eval", "l0", "l1", "spr", "exp", "total" };

typedef struct {
    uint32_t max[ST_COUNT];
    uint64_t band_sum[ST_COUNT];     // over the sprite band's lines
    uint64_t all_sum[ST_COUNT];      // over every line
    uint32_t band_n, all_n;
    uint64_t check_cyc, check_us;    // an uninstrumented pass: cycles against wall time
} result_t;

static spike_t v;
static spike_line_t ln;
static uint16_t rgb[640];
static spike_scene_t scenes[80];
static result_t results[80];
static unsigned nscenes;

static inline uint32_t cyc(void) { return m33_hw->dwt_cyccnt; }

static void dwt_start(void) {    // per core: the DWT is in each core's private space
    m33_hw->demcr |= M33_DEMCR_TRCENA_BITS;
    m33_hw->dwt_cyccnt = 0;
    m33_hw->dwt_ctrl |= M33_DWT_CTRL_CYCCNTENA_BITS;
}

// ---------------------------------------------------------------------------
// Stand-ins for what the spike has no real code for yet: the bus interrupt
// (Phase 11) and vdp_line_start's journal drain (Phase 3). Shaped like the
// work PLAN.md section 3 describes, so the cost is the right order.

typedef struct { uint16_t ptr; uint8_t prefetch, flip, payload, statsel; } port_t;
static port_t ports[2];
static uint8_t bus_vram[0x10000];
static uint8_t registers[128], render_registers[128];
static uint32_t journal[1024];
static unsigned journal_n;
static int8_t vinc = 1;
static uint16_t palbase = 0xFC00;
static uint8_t stat[16];
enum { OP_DATA_WRITE, OP_DATA_READ, OP_REG_WRITE, OP_STATUS_READ, OP_COUNT };
static const char *const op_names[OP_COUNT] = { "data-write", "data-read", "reg-write", "status-read" };
static unsigned bus_op;
static uint32_t bus_word = 0x8000005A;

static void __not_in_flash_func(bus_isr)(void) {
    const uint32_t fifo = pio0->rxf[3];                  // the FIFO pop, an empty SM's
    (void)fifo;
    const uint32_t w = bus_word;
    port_t *const p = &ports[w >> 31];                   // MODE1 picks the port
    const uint8_t value = (uint8_t)w;
    switch (bus_op) {
    case OP_DATA_WRITE: {
        const uint16_t a = p->ptr;
        bus_vram[a] = value;
        journal[journal_n++ & 1023] = a | (uint32_t)value << 16;
        p->prefetch = value;
        p->ptr = (uint16_t)(a + vinc);
        p->flip = 0;
        break;
    }
    case OP_DATA_READ: {
        const uint16_t a = p->ptr;
        p->prefetch = bus_vram[a];
        p->ptr = (uint16_t)(a + vinc);
        p->flip = 0;
        break;
    }
    case OP_REG_WRITE:       // the second byte of a command: register r gets the payload
        if (value & 0x80) {
            const unsigned r = value & 0x7F;
            registers[r] = p->payload;
            if (r == 0x09) vinc = (int8_t)p->payload;
            if (r == 0x0C) palbase = (uint16_t)((p->payload & 0x3F) << 10);
        }
        p->flip = 0;
        break;
    case OP_STATUS_READ:     // acknowledge STAT0: flags and latches clear
        if ((p->statsel & 15) == 0) { stat[0] = 0; stat[1] &= ~0x0D; }
        p->flip = 0;
        break;
    }
    // Restage the four bytes the read program answers from.
    pio0->txf[3] = ports[0].prefetch | (uint32_t)stat[ports[0].statsel & 15] << 8 |
                   (uint32_t)ports[1].prefetch << 16 | (uint32_t)stat[ports[1].statsel & 15] << 24;
}

static uint32_t line_start(void) {
    // Registers snapshot, and the journal drained into the render copy with
    // the palette window snooped: v.vram is the render copy here.
    const uint32_t t0 = cyc();
    memcpy(render_registers, registers, sizeof registers);
    const unsigned n = journal_n < 1024 ? journal_n : 1024;
    for (unsigned i = 0; i < n; i++) {
        const uint32_t e = journal[i];
        const uint16_t a = (uint16_t)e;
        v.vram[a] = (uint8_t)(e >> 16);
        const uint16_t off = (uint16_t)(a - palbase);
        if (off < 512) {
            const uint16_t base = palbase + (off & ~1u);
            const uint32_t rgb12 = (v.vram[base] & 15u) | (uint32_t)v.vram[(uint16_t)(base + 1)] << 4;
            v.pal2[off >> 1] = rgb12 | rgb12 << 16;
        }
    }
    journal_n = 0;
    return cyc() - t0;
}

// ---------------------------------------------------------------------------

static void run_scene(unsigned i) {
    result_t *r = &results[i];
    memset(r, 0, sizeof *r);
    spike_scene_build(&v, &scenes[i]);

    const uint32_t irq = save_and_disable_interrupts();
    for (unsigned frame = 0; frame < FRAMES; frame++) {
        spike_scene_frame(&v, frame);
        v.ovf = v.col = false;
        v.colmap = 0;
        for (int line = 0; line < 240; line++) {
            const uint32_t t0 = cyc();
            spike_sprite_eval(&v, &ln, line);
            const uint32_t t1 = cyc();
            spike_layer0(&v, &ln, line);
            const uint32_t t2 = cyc();
            spike_layer1(&v, &ln, line);
            const uint32_t t3 = cyc();
            spike_sprites(&v, &ln, line);
            const uint32_t t4 = cyc();
            spike_finish(&v, &ln, rgb);
            const uint32_t t5 = cyc();

            const uint32_t d[ST_COUNT] = { t1 - t0, t2 - t1, t3 - t2, t4 - t3, t5 - t4, t5 - t0 };
            const bool band = spike_in_sprite_band(&v, line);
            for (unsigned s = 0; s < ST_COUNT; s++) {
                if (d[s] > r->max[s]) r->max[s] = d[s];
                r->all_sum[s] += d[s];
                if (band) r->band_sum[s] += d[s];
            }
            r->all_n++;
            if (band) r->band_n++;
        }
    }

    // The same builds again with only the frame timed, both ways: does the
    // counter count what the clock says it should?
    const uint64_t u0 = time_us_64();
    const uint32_t c0 = cyc();
    uint64_t cycles = 0;
    uint32_t last = c0;
    for (unsigned frame = 0; frame < FRAMES; frame++) {
        spike_scene_frame(&v, frame);
        for (int line = 0; line < 240; line++) spike_build_line(&v, &ln, line, rgb);
        const uint32_t now = cyc();          // 32 bits wrap in 14 s at 302.4 MHz
        cycles += now - last;
        last = now;
    }
    r->check_us = time_us_64() - u0;
    r->check_cyc = cycles;
    restore_interrupts(irq);
}

typedef struct { uint32_t max, probe; uint64_t sum; } micro_t;
static micro_t bus_results[OP_COUNT], drain_result, probe_result;

static void run_micro(void) {
    // The cost of reading the counter itself.
    probe_result = (micro_t){ 0 };
    for (unsigned i = 0; i < 10000; i++) {
        const uint32_t t0 = cyc(), t1 = cyc();
        const uint32_t d = t1 - t0;
        if (d > probe_result.max) probe_result.max = d;
        probe_result.sum += d;
    }

    // Bus interrupt service, entry and exit included: pend a spare IRQ on this
    // core and count across it. The handler goes straight in the vector table.
    irq_set_exclusive_handler(SPARE_IRQ_0, bus_isr);
    irq_set_enabled(SPARE_IRQ_0, true);
    for (unsigned op = 0; op < OP_COUNT; op++) {
        micro_t *m = &bus_results[op];
        *m = (micro_t){ 0 };
        bus_op = op;
        for (unsigned i = 0; i < 10000; i++) {
            bus_word = (i & 1u) << 31 | (op == OP_REG_WRITE ? 0x89 : (uint8_t)i);
            journal_n &= 63;
            const uint32_t t0 = cyc();
            irq_set_pending(SPARE_IRQ_0);
            const uint32_t d = cyc() - t0;
            if (d > m->max) m->max = d;
            m->sum += d;
        }
    }
    irq_set_enabled(SPARE_IRQ_0, false);

    // vdp_line_start with 32 journal entries — a 2 MHz CPU's whole line of
    // back-to-back writes — all of them palette writes, the worst case.
    drain_result = (micro_t){ 0 };
    spike_scene_build(&v, &scenes[0]);
    const uint32_t irq = save_and_disable_interrupts();
    for (unsigned i = 0; i < 10000; i++) {
        for (unsigned k = 0; k < 32; k++)
            journal[k] = (uint32_t)(palbase + ((i + k) & 511)) | (uint32_t)(uint8_t)(i * 7 + k) << 16;
        journal_n = 32;
        const uint32_t d = line_start();
        if (d > drain_result.max) drain_result.max = d;
        drain_result.sum += d;
    }
    restore_interrupts(irq);
}

// ---------------------------------------------------------------------------
// The split suite

// Interrupt load. The bus: a PWM wrap interrupt on core 1 every 2 µs — a 2 MHz
// 6502's back-to-back sta abs, without pause — doing a data write into the
// palette window, so every line start drains a full journal through the
// palette cache. VGA: a PWM wrap interrupt on core 0 at the VGA line rate,
// shaped like pico9918's dmaIrqHandler (ack, count the line, re-arm, and every
// other line hand one to the renderer).
#define BUS_SLICE 1
#define VGA_SLICE 0
#define SPLIT_PALBASE 0x3000        // clear of every table the scenes use

static volatile uint32_t bus_count;
static volatile uint32_t vga_count, vga_max;
static unsigned vga_line;

static void __not_in_flash_func(bus_pwm_isr)(void) {
    pwm_hw->intr = 1u << BUS_SLICE;
    const uint32_t w = bus_word++;
    port_t *const p = &ports[0];
    const uint16_t a = p->ptr;
    bus_vram[a] = (uint8_t)w;
    journal[journal_n++ & 1023] = a | (uint32_t)(uint8_t)w << 16;
    p->prefetch = (uint8_t)w;
    p->ptr = (uint16_t)(palbase + ((a + vinc - palbase) & 511));
    p->flip = 0;
    pio0->txf[3] = ports[0].prefetch | (uint32_t)stat[ports[0].statsel & 15] << 8 |
                   (uint32_t)ports[1].prefetch << 16 | (uint32_t)stat[ports[1].statsel & 15] << 24;
    bus_count++;
}

static void __not_in_flash_func(vga_pwm_isr)(void) {
    const uint32_t t0 = cyc();
    pwm_hw->intr = 1u << VGA_SLICE;
    (void)dma_hw->ints0;
    vga_line = vga_line + 1 == 525 ? 0 : vga_line + 1;
    pio0->txf[2] = vga_line;
    if (vga_line & 1) {
        (void)sio_hw->fifo_st;
        pio0->txf[2] = vga_line | 0x1000;
    }
    vga_count++;
    const uint32_t d = cyc() - t0;
    if (d > vga_max) vga_max = d;
}

// Start a PWM slice interrupting the calling core at about `rate` Hz.
static void pwm_irq_start(unsigned slice, uint32_t rate, uint32_t div, bool irq1, irq_handler_t handler) {
    pwm_config c = pwm_get_default_config();
    pwm_config_set_clkdiv_int(&c, div);
    pwm_config_set_wrap(&c, (uint16_t)(clock_get_hz(clk_sys) / div / rate - 1));
    pwm_init(slice, &c, false);
    const unsigned irq = irq1 ? PWM_IRQ_WRAP_1 : PWM_IRQ_WRAP_0;
    irq_set_exclusive_handler(irq, handler);
    if (irq1) pwm_set_irq1_enabled(slice, true);
    else pwm_set_irq0_enabled(slice, true);
    pwm_clear_irq(slice);
    irq_set_enabled(irq, true);
    pwm_set_enabled(slice, true);
}

static void pwm_irq_stop(unsigned slice, bool irq1) {
    pwm_set_enabled(slice, false);
    irq_set_enabled(irq1 ? PWM_IRQ_WRAP_1 : PWM_IRQ_WRAP_0, false);
    if (irq1) pwm_set_irq1_enabled(slice, false);
    else pwm_set_irq0_enabled(slice, false);
    pwm_clear_irq(slice);
}

// Core 0's half of a line. The handoff is the inter-core FIFO, and both cores
// wait by polling its status in SIO, which is core-local: a wait loop reading
// SRAM would contend with the other core's work on the bus fabric.
#define JOB_STOP 0xFFFFFFFFu
typedef struct {
    int line, xs;
    uint32_t work;
} job_t;
static job_t job;
static spike_sprline_t sprline;

static void __not_in_flash_func(core0_worker)(void) {
    dwt_start();
    for (;;) {
        while (!multicore_fifo_rvalid()) {}
        if (multicore_fifo_pop_blocking() == JOB_STOP) return;
        const uint32_t t0 = cyc();
        const int xs = job.xs;
        spike_sprites_split(&v, ln.list, ln.nlist, job.line, 0, xs, &sprline);
        job.work = cyc() - t0;
        multicore_fifo_push_blocking(1);
    }
}

// Mode 0 is single-core; mode m in 1..6 splits at xs = W − 32(m − 1), down to half
// the picture. Two diagnostics of where core 0's time goes, each with all the
// sprites on one core: SM_C0_ALONE, on core 0 while core 1 only waits;
// SM_C1_ALONE, the same code on core 1 with interrupts off.
#define SM_C0_ALONE 7
#define SM_C1_ALONE 8
#define SM_AUTO 9           // xs chosen per line by spike_split_choose
#define SM_COUNT 10
static inline int split_xs(int W, unsigned mode) { return W - 32 * (int)(mode - 1); }

typedef struct {
    uint32_t lat_max, wait_max, core1_max, core0_max;
    uint64_t lat_sum;
    uint32_t lines, bus, late;
    int xs;
} split_result_t;
static split_result_t split_results[80][SM_COUNT];
static unsigned split_scene_count;
static uint8_t split_scene_index[80];

static void run_split(unsigned si, unsigned mode) {
    split_result_t *r = &split_results[si][mode];
    *r = (split_result_t){ 0 };
    spike_scene_build(&v, &scenes[split_scene_index[si]]);
    const int W = v.width;
    const bool diag = mode == SM_C0_ALONE || mode == SM_C1_ALONE;
    int xs = mode && !diag && mode != SM_AUTO ? split_xs(W, mode) : W;
    r->xs = mode == SM_C0_ALONE ? -2 : mode == SM_C1_ALONE ? -3 : mode == SM_AUTO ? 0 : mode ? xs : -1;
    uint32_t layer_cycles = 0;
    int bias = 0;
    const uint32_t budget = (uint32_t)((uint64_t)clock_get_hz(clk_sys) * 63556 / 1000000000);
    journal_n = 0;
    const uint32_t bus0 = bus_count;

    for (unsigned frame = 0; frame < FRAMES; frame++) {
        spike_scene_frame(&v, frame);
        v.ovf = v.col = false;
        v.colmap = 0;
        for (int line = 0; line < 240; line++) {
            const uint32_t t0 = cyc();
            // The latch: equal priority with the bus, so nothing preempts the drain.
            const uint32_t irq = save_and_disable_interrupts();
            line_start();
            restore_interrupts(irq);
            spike_sprite_eval(&v, &ln, line);
            uint32_t wait = 0;
            if (mode == SM_C1_ALONE) {
                const uint32_t irq2 = save_and_disable_interrupts();
                const uint32_t t = cyc();
                spike_sprites_split(&v, ln.list, ln.nlist, line, 0, W, &sprline);
                const uint32_t d = cyc() - t;
                restore_interrupts(irq2);
                if (d > r->core0_max) r->core0_max = d;
                spike_layer0(&v, &ln, line);
                spike_layer1(&v, &ln, line);
                spike_sprite_merge(&v, &ln, &sprline);
            } else if (mode == 0) {
                spike_layer0(&v, &ln, line);
                spike_layer1(&v, &ln, line);
                spike_sprites(&v, &ln, line);
            } else {
                if (mode == SM_AUTO) xs = spike_split_choose(&v, &ln, layer_cycles, bias);
                job.line = line;
                job.xs = xs;
                const uint32_t tpost = cyc();
                multicore_fifo_push_blocking(0);
                if (mode != SM_C0_ALONE) {
                    const uint32_t tl = cyc();
                    spike_layer0(&v, &ln, line);
                    spike_layer1(&v, &ln, line);
                    layer_cycles = cyc() - tl;
                    if (xs < W) spike_sprites_range(&v, &ln, line, xs, W);
                }
                const uint32_t tw = cyc();
                const uint32_t par1 = tw - tpost;
                while (!multicore_fifo_rvalid()) {}
                multicore_fifo_pop_blocking();
                wait = cyc() - tw;
                spike_sprite_merge(&v, &ln, &sprline);
                if (job.work > r->core0_max) r->core0_max = job.work;
                // Feedback: move the boundary a column toward the core that finished first.
                if (mode == SM_AUTO && ln.nlist) {
                    if (job.work > par1 + 1000 && bias > -96) bias -= 32;
                    else if (par1 > job.work + 1000 && bias < 96) bias += 32;
                }
            }
            spike_finish(&v, &ln, rgb);
            const uint32_t lat = cyc() - t0;
            if (lat > r->lat_max) r->lat_max = lat;
            if (lat > budget) r->late++;
            if (wait > r->wait_max) r->wait_max = wait;
            if (lat - wait > r->core1_max) r->core1_max = lat - wait;
            r->lat_sum += lat;
            r->lines++;
        }
    }
    r->bus = bus_count - bus0;
}

static void split_suite(void) {
    // Every depth and sprite setting, both geometries, table on, SPRLIMIT 32; and
    // Full mode 4bpp at SPRLIMIT 16, the split with Still Open 1's second remedy.
    split_scene_count = 0;
    for (unsigned i = 0; i < nscenes; i++)
        if (scenes[i].tab4 && !scenes[i].single &&
            (scenes[i].limit == 32 || (scenes[i].limit == 16 && scenes[i].geom == SPIKE_FULL)))
            split_scene_index[split_scene_count++] = (uint8_t)i;

    palbase = SPLIT_PALBASE;
    ports[0].ptr = SPLIT_PALBASE;
    pwm_irq_start(BUS_SLICE, 500000, 1, true, bus_pwm_isr);
    for (unsigned si = 0; si < split_scene_count; si++) {
        const int W = scenes[split_scene_index[si]].geom == SPIKE_FULL ? 320 : 256;
        for (unsigned m = 0; m < SM_COUNT; m++) {
            if ((m == SM_C0_ALONE || m == SM_C1_ALONE) && scenes[split_scene_index[si]].depth != SPIKE_4BPP) {
                split_results[si][m].lines = 0;
                continue;
            }
            if (m && m < SM_C0_ALONE && split_xs(W, m) < W / 2) {   // (xs=0 in the output is SM_AUTO)
                split_results[si][m].lines = 0;
                continue;
            }
            run_split(si, m);
        }
    }
    pwm_irq_stop(BUS_SLICE, true);
    palbase = 0xFC00;
}

static void core1_main(void) {
    dwt_start();
    for (;;) {
        const uint32_t cmd = multicore_fifo_pop_blocking();
        if (cmd == 's') {
            split_suite();
            multicore_fifo_push_blocking(JOB_STOP);
            continue;
        }
        for (unsigned i = 0; i < nscenes; i++) run_scene(i);
        run_micro();
        multicore_fifo_push_blocking(1);
    }
}

static void set_clock(const clock_preset_t *c) {
    // Raise the voltage before the clock, lower it after.
    if (c->voltage > vreg_get_voltage()) {
        vreg_set_voltage(c->voltage);
        sleep_ms(10);
    }
    set_sys_clock_pll(c->pll, c->div1, c->div2);
    if (c->voltage < vreg_get_voltage()) vreg_set_voltage(c->voltage);
    sleep_ms(100);
}

static void report(uint32_t hz) {
    printf("CLOCK hz=%lu opt=%s budget=%lu\n", (unsigned long)hz, PICOVDP_SPIKE_OPT[0] ? PICOVDP_SPIKE_OPT : "preset",
           (unsigned long)((uint64_t)hz * 63556 / 1000000000));   // cycles in one display line, 63.556 µs
    for (unsigned i = 0; i < nscenes; i++) {
        const result_t *r = &results[i];
        printf("SCENE hz=%lu name=%s", (unsigned long)hz, scenes[i].name);
        for (unsigned s = 0; s < ST_COUNT; s++)
            printf(" %s_max=%lu %s_band=%lu %s_mean=%lu", stage_names[s], (unsigned long)r->max[s],
                   stage_names[s], (unsigned long)(r->band_sum[s] / r->band_n),
                   stage_names[s], (unsigned long)(r->all_sum[s] / r->all_n));
        // The uninstrumented pass: µs ×1000 for all its builds, from cycles and from the timer.
        printf(" check_cyc_us1000=%llu check_wall_us1000=%llu\n",
               (unsigned long long)(r->check_cyc * 1000000000ull / hz), (unsigned long long)(r->check_us * 1000));
    }
    printf("MICRO hz=%lu name=probe max=%lu mean=%lu\n", (unsigned long)hz, (unsigned long)probe_result.max,
           (unsigned long)(probe_result.sum / 10000));
    for (unsigned op = 0; op < OP_COUNT; op++)
        printf("MICRO hz=%lu name=bus-%s max=%lu mean=%lu\n", (unsigned long)hz, op_names[op],
               (unsigned long)bus_results[op].max, (unsigned long)(bus_results[op].sum / 10000));
    printf("MICRO hz=%lu name=line-start-32 max=%lu mean=%lu\n", (unsigned long)hz, (unsigned long)drain_result.max,
           (unsigned long)(drain_result.sum / 10000));
}

static void report_split(uint32_t hz) {
    printf("CLOCK hz=%lu opt=%s budget=%lu vga_max=%lu vga_count=%lu\n", (unsigned long)hz,
           PICOVDP_SPIKE_OPT[0] ? PICOVDP_SPIKE_OPT : "preset",
           (unsigned long)((uint64_t)hz * 63556 / 1000000000), (unsigned long)vga_max, (unsigned long)vga_count);
    for (unsigned si = 0; si < split_scene_count; si++) {
        for (unsigned m = 0; m < SM_COUNT; m++) {
            const split_result_t *r = &split_results[si][m];
            if (!r->lines) continue;
            printf("SPLIT hz=%lu name=%s xs=%d late=%lu lat_max=%lu lat_mean=%lu wait_max=%lu core1_max=%lu core0_max=%lu bus_per_line_x100=%lu\n",
                   (unsigned long)hz, scenes[split_scene_index[si]].name, r->xs, (unsigned long)r->late, (unsigned long)r->lat_max,
                   (unsigned long)(r->lat_sum / r->lines), (unsigned long)r->wait_max, (unsigned long)r->core1_max,
                   (unsigned long)r->core0_max, (unsigned long)(100ull * r->bus / r->lines));
        }
    }
}

int main(void) {
    stdio_init_all();
    spike_init_tables();
    nscenes = spike_scene_list(scenes, 80);
    multicore_launch_core1(core1_main);

    for (;;) {
        printf("picovdp spike ready: send r or s\n");
        int ch;
        do ch = getchar_timeout_us(2000000); while (ch != 'r' && ch != 's' && ch != PICO_ERROR_TIMEOUT);
        if (ch == PICO_ERROR_TIMEOUT) continue;

        if (ch == 's') {
            printf("BEGIN split frames=%u lines=240\n", FRAMES);
            for (unsigned c = 1; c < sizeof presets / sizeof presets[0]; c++) {    // 352 MHz, the preset chosen
                set_clock(&presets[c]);
                const uint32_t hz = clock_get_hz(clk_sys);
                vga_max = vga_count = 0;
                pwm_irq_start(VGA_SLICE, 31469, 16, false, vga_pwm_isr);
                multicore_fifo_push_blocking('s');
                core0_worker();                 // until core 1 has finished the suite
                pwm_irq_stop(VGA_SLICE, false);
                report_split(hz);
            }
            printf("END\n");
            continue;
        }

        printf("BEGIN scenes=%u frames=%u lines=240\n", nscenes, FRAMES);
        for (unsigned c = 0; c < sizeof presets / sizeof presets[0]; c++) {
            set_clock(&presets[c]);
            const uint32_t hz = clock_get_hz(clk_sys);
            multicore_fifo_push_blocking(1);
            multicore_fifo_pop_blocking();
            report(hz);
        }
        printf("END\n");
    }
}
