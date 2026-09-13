// Phase 1 timing spike, RP2350 side (PLAN.md section 6, Phase 1).
//
// Core 1 builds every scene's lines and times each stage with the M33's DWT
// cycle counter, interrupts off. Core 0 owns USB and the clock: send 'r' over
// the CDC port and it runs the suite at 302.4 MHz and then 352 MHz, printing
// one machine-readable line per result (spike/report.mjs turns them into
// tables), and ends with "END".

#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
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
    const unsigned n = journal_n;
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

static void core1_main(void) {
    dwt_start();
    for (;;) {
        multicore_fifo_pop_blocking();
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

int main(void) {
    stdio_init_all();
    spike_init_tables();
    nscenes = spike_scene_list(scenes, 80);
    multicore_launch_core1(core1_main);

    for (;;) {
        printf("picovdp spike ready: send r\n");
        int ch;
        do ch = getchar_timeout_us(2000000); while (ch != 'r' && ch != PICO_ERROR_TIMEOUT);
        if (ch != 'r') continue;

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
