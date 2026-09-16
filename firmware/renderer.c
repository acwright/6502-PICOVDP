// The card on two cores. See renderer.h.

#include "renderer.h"

#include <string.h>

#include "hardware/irq.h"
#include "hardware/structs/m33.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "vga.h"

#if PICOVDP_DEBUG
#include "hardware/clocks.h"
#include "hardware/pwm.h"
#include "fault.h"
#include "inject.h"
#include "profile.h"
#include "scenes.h"
#endif

// The card: about 138 KB, in SRAM.
static vdp_t card;

// ---- the rows' buffers ----
//
// Four, so a finished row can wait while another is sent and a third is built:
// core 1 builds into one that is neither being sent nor among the last two
// finished, and core 0 only ever starts sending the latest finished one.

#define BUFFERS 4
#define PICTURE_ROWS VDP_HEIGHT

typedef struct row_buffer {
    uint8_t indices[VDP_WIDTH];
    uint32_t words[VGA_RGB_WORDS];  // 640 pixels, two to a word, and the guard word
} row_buffer_t;

static row_buffer_t buffers[BUFFERS];

// The latest finished row: bit 31 set once there is one, the row in 23:8, the buffer in 7:0.
#define READY 0x80000000u
static volatile uint32_t ready;
static volatile uint32_t previous_ready;
static volatile uint8_t shown;      // the buffer core 0 is sending

// ---- line starts, core 0 to core 1 ----
//
// Core 0 writes the line's frame and screen line at events[count], then counts
// it and rings. Core 1's latch takes every event up to the count, in order, and
// tags each latch with its event number, which comes back with the render side.

#define EVENTS 256
typedef struct line_event {
    uint32_t frame;
    uint16_t screen_line;
} line_event_t;

static line_event_t events[EVENTS];
static uint32_t latch_cycles[EVENTS];   // core 1's DWT count as each latch was taken
static volatile uint32_t events_begun;  // core 0's
static uint32_t events_latched;         // core 1's latch interrupt's
static uint32_t frames;                 // core 0's: frames begun
static int bell = -1;

static volatile uint32_t heartbeat;
static volatile bool core1_ready;

// ---- core 0's half of a row ----
//
// Core 1 posts the split through the inter-core FIFO. Core 0 builds picture
// columns [0, split) — both layers and the sprites — into a half of its own and
// expands them into the row's buffer, then posts back what it took. Core 1
// meanwhile does the same for the rest of the row. The halves share nothing but
// the render side, which both only read, and the buffer, whose words each
// writes only for its own columns.

static vdp_half_t core0_half;
static uint32_t *volatile job_words;

static inline uint32_t cycles(void) {
    return m33_hw->dwt_cyccnt;
}

static void cycle_counter_start(void) {  // each core's DWT is its own
    m33_hw->demcr |= M33_DEMCR_TRCENA_BITS;
    m33_hw->dwt_cyccnt = 0;
    m33_hw->dwt_ctrl |= M33_DWT_CTRL_CYCCNTENA_BITS;
}

#if PICOVDP_DEBUG

// ---- statistics ----

typedef struct stats_raw {
    uint32_t rows_built, lines_taken, late_rows, missed_bells;
    uint64_t latency_sum, build_sum, split_sum;
    uint32_t split_rows;
    uint32_t latency_max, build_max;
    uint32_t catch_up_max, half_max, wait_max, publish_max, expand_max, core0_max;
    uint32_t latch_isr_max, line_isr_max, bus_standins;
    uint32_t merged_base, overflows_base, slips_base;
    uint16_t histogram[RENDERER_HISTOGRAM_BINS];
} stats_raw_t;

static stats_raw_t stats;
static volatile bool stats_reset_request;

static inline void maximum(uint32_t *max, uint32_t value) {
    if (value > *max) *max = value;
}

// ---- snapshots ----

// ARMED: waiting for the frame. READY: its first row's latch has passed, so
// core 1 keeps each row's indices from here. RUNNING: its rows are going to VGA.
enum { CAPTURE_IDLE, CAPTURE_ARMED, CAPTURE_READY, CAPTURE_RUNNING, CAPTURE_DONE };
static volatile uint8_t capture_state;
static volatile uint32_t capture_frame;  // 0xffffffff: the next frame to start
static renderer_snapshot_t captured;
static renderer_snapshot_t *const capture = &captured;

// The state as the last row 239 finished, under a sequence count: odd while written.
static volatile uint32_t state_sequence;
static renderer_state_t state_at_239;

// ---- requests from core 0's thread to core 1's, taken between lines ----

enum { REQUEST_NONE, REQUEST_RESET, REQUEST_VRAM, REQUEST_SCENE, REQUEST_LOAD, REQUEST_FAULT, REQUEST_PROFILE };
typedef struct request {
    uint8_t kind;
    bool power_on;
    unsigned scene;
    unsigned fault;
    renderer_load_t load;
    uint8_t *vram;
    uint16_t row;
    uint32_t iterations;
    profile_t *profile;
} request_t;

static request_t request;
static volatile uint32_t request_sequence, request_done;

// ---- loads and scenes ----

static renderer_load_t load;
static int scene = -1;
static bool scene_pending;
static uint32_t scene_frame_count;
static renderer_scene_frame_t scene_log[64];
static volatile uint32_t scene_log_head, scene_log_tail;

#define BUS_SLICE 1
static uint32_t bus_writes;

#endif

// ============================================================================
// Core 0

// A screen line begins (vga.h). Rings core 1, and picks the buffer a row sends.
static const uint32_t *__time_critical_func(line_start)(uint16_t screen_line) {
#if PICOVDP_DEBUG
    const uint32_t t0 = cycles();
#endif
    if (screen_line == 0) frames++;
#if PICOVDP_DEBUG
    // The latch that starts a captured frame's first row: from here core 1
    // keeps the indices of the rows it builds.
    if (screen_line == VDP_SCREEN_LINES - 1 && capture_state == CAPTURE_ARMED &&
        (capture_frame == 0xffffffffu || capture_frame == frames + 1)) {
        capture_state = CAPTURE_READY;
    }
#endif
    const uint32_t n = events_begun;
    events[n & (EVENTS - 1)] = (line_event_t){frames, screen_line};
    events_begun = n + 1;
    multicore_doorbell_set_other_core((uint)bell);

    const uint32_t *sending = NULL;
    if (screen_line < PICTURE_ROWS) {
        const uint32_t r = ready;
        uint8_t b = (uint8_t)r;
        bool on_time = (r & READY) && ((r >> 8) & 0xffff) == screen_line;
        if (!(r & READY)) b = shown;
        shown = b;
        sending = buffers[b].words;
#if PICOVDP_DEBUG
        if (!on_time) stats.late_rows++;
        if (screen_line == 0 && capture_state == CAPTURE_READY) {
            capture->frame = frames;
            capture_state = CAPTURE_RUNNING;
        }
        if (capture_state == CAPTURE_RUNNING) {
            memcpy(capture->rows + screen_line * VDP_WIDTH, buffers[b].indices, VDP_WIDTH);
            capture->source[screen_line] = (uint8_t)(r >> 8);
            capture->late[screen_line] = !on_time;
            if (screen_line == PICTURE_ROWS - 1) capture_state = CAPTURE_DONE;
        }
#else
        (void)on_time;
#endif
    }
#if PICOVDP_DEBUG
    maximum(&stats.line_isr_max, cycles() - t0);
#endif
    return sending;
}

// Core 0's half of a row, above; posts back the cycles it took.
static void __isr __time_critical_func(half_job_isr)(void) {
    while (multicore_fifo_rvalid()) {
        const int split = (int)sio_hw->fifo_rd;
        const uint32_t t0 = cycles();
        vdp_build_half(&card, &core0_half, 0, split);
        vdp_expand_half(&card, &core0_half, (uint16_t *)job_words);
        sio_hw->fifo_wr = cycles() - t0;
    }
    multicore_fifo_clear_irq();
}

// ============================================================================
// Core 1

// §3's latch: every line begun since the last, in order.
static void __isr __time_critical_func(latch_isr)(void) {
    const uint32_t now = cycles();
    multicore_doorbell_clear_current_core((uint)bell);
    const uint32_t begun = events_begun;
#if PICOVDP_DEBUG
    if (begun - events_latched > 1) stats.missed_bells++;
    const bool thread_latches = inject_active();
#else
    const bool thread_latches = false;
#endif
    while (events_latched != begun) {
        const uint32_t n = events_latched++;
        latch_cycles[n & (EVENTS - 1)] = now;
        if (!thread_latches) vdp_latch(&card, events[n & (EVENTS - 1)].screen_line, n);
    }
#if PICOVDP_DEBUG
    maximum(&stats.latch_isr_max, cycles() - now);
#endif
}

// A buffer no line start can be sending or about to send.
static unsigned free_buffer(void) {
    const uint32_t r = ready, p = previous_ready;
    const uint8_t s = shown;
    for (unsigned b = 0; b < BUFFERS; b++) {
        if (b == s) continue;
        if ((r & READY) && b == (uint8_t)r) continue;
        if ((p & READY) && b == (uint8_t)p) continue;
        return b;
    }
    return 0;  // unreachable with four
}

// The column the cores divide a row at: the core's estimate, moved toward
// whichever core finished first on the last row.
static int split_bias;

static int split_for_row(void) {
    int split = vdp_split_choose(&card);
    if (!split) return 0;
    split += split_bias;  // both multiples of 8
    if (split < 8) split = 8;
    if (split > VDP_WIDTH) split = VDP_WIDTH;
    return split;
}

#if PICOVDP_DEBUG

static void apply_request(void);
static void reset_stats(void);
static void scene_line(void);
static void pad_build(uint16_t row, uint32_t t0);

static void save_state(uint32_t frame) {
    state_sequence++;
    const uint32_t irq = save_and_disable_interrupts();
    // vdp_debug_save copies VRAM too; the state carries none, so take the fields.
    vdp_snapshot_t *s = &state_at_239.card;
    for (unsigned i = 0; i < VDP_REGISTERS; i++) s->registers[i] = card.reg[i];
    s->port[0] = card.port[0];
    s->port[1] = card.port[1];
    s->screen_line = card.screen_line;
    s->display_line = card.display_line;
    s->stat0 = card.stat0;
    s->irq_latch = card.irq_latch;
    s->frame_events = card.frame_events;
    s->overflow_sprite = card.overflow_sprite;
    memcpy(s->collision_map, card.collision_map, sizeof s->collision_map);
    s->font_pending = card.font_pending;
    memcpy(s->font_id, card.font_id, sizeof s->font_id);
    memcpy(s->font_base, card.font_base, sizeof s->font_base);
    state_at_239.interrupt = vdp_int_asserted(&card);
    restore_interrupts(irq);
    state_at_239.frame = frame;
    state_at_239.scene_frame = scene_frame_count;
    state_sequence++;
}

#endif

// The row the render side has just caught up with: built, published, handed
// to core 0. `n` is the latch's event number.
static void __time_critical_func(render_line)(uint32_t n) {
    const uint16_t row = card.render_screen_line;
    const uint32_t t0 = cycles();
    unsigned b = 0;
    uint32_t core0_work = 0, t_built = t0, t_expand = t0, t_wait = t0;
    bool posted = false;

    if (row < PICTURE_ROWS) {
        b = free_buffer();
        const int split = split_for_row();
        posted = split > 0;
        if (posted) {
            job_words = buffers[b].words;
            multicore_fifo_push_blocking((uint32_t)split);
        }
        vdp_build_half(&card, &card.half, split, VDP_WIDTH);
#if PICOVDP_DEBUG
        pad_build(row, t0);
#endif
        t_built = cycles();
        vdp_expand_half(&card, &card.half, (uint16_t *)buffers[b].words);
        t_expand = cycles();
        if (posted) {
            while (!multicore_fifo_rvalid()) {
            }
            core0_work = sio_hw->fifo_rd;
            // Feedback: move the split toward the core that finished first.
            const uint32_t core1_work = t_expand - t0;
            if (core0_work > core1_work + 300 && split_bias > -160) split_bias -= 8;
            else if (core1_work > core0_work + 300 && split_bias < 160) split_bias += 8;
        }
        t_wait = cycles();
#if PICOVDP_DEBUG
        if (posted) {
            stats.split_sum += (uint32_t)split;
            stats.split_rows++;
        }
        // A snapshot records indices as they went to VGA (DEBUGLINK.md): kept
        // with the row's buffer, and only while one is being taken.
        if (capture_state == CAPTURE_READY || capture_state == CAPTURE_RUNNING) {
            if (posted) vdp_copy_half(&card, &core0_half, buffers[b].indices);
            vdp_copy_half(&card, &card.half, buffers[b].indices);
        }
#endif
    }

    uint32_t irq = save_and_disable_interrupts();
    vdp_publish(&card, posted ? &core0_half : NULL, row < PICTURE_ROWS ? &card.half : NULL);
    restore_interrupts(irq);
    const uint32_t t_publish = cycles();

    if (row < PICTURE_ROWS) {
        previous_ready = ready;
        ready = READY | (uint32_t)row << 8 | b;
    }
    const uint32_t t_done = cycles();
    heartbeat++;

#if PICOVDP_DEBUG
    stats.lines_taken++;
    if (row < PICTURE_ROWS) {
        const uint32_t latency = t_done - latch_cycles[n & (EVENTS - 1)];
        const uint32_t build = t_done - t0;
        stats.rows_built++;
        stats.latency_sum += latency;
        stats.build_sum += build;
        maximum(&stats.latency_max, latency);
        maximum(&stats.build_max, build);
        uint32_t bin = latency >> RENDERER_HISTOGRAM_SHIFT;
        if (bin >= RENDERER_HISTOGRAM_BINS) bin = RENDERER_HISTOGRAM_BINS - 1;
        if (stats.histogram[bin] != 0xffff) stats.histogram[bin]++;
        maximum(&stats.half_max, t_built - t0);
        maximum(&stats.expand_max, t_expand - t_built);
        maximum(&stats.wait_max, t_wait - t_expand);
        maximum(&stats.core0_max, core0_work);
    }
    maximum(&stats.publish_max, t_publish - (row < PICTURE_ROWS ? t_wait : t0));
    if (row == PICTURE_ROWS - 1) save_state(events[n & (EVENTS - 1)].frame);
#else
    // The stages are timed for the statistics alone.
    (void)n, (void)t_built, (void)t_wait, (void)t_publish, (void)t_done;
#endif
}

static void __time_critical_func(core1_main)(void) {
    cycle_counter_start();
    irq_set_exclusive_handler(SIO_IRQ_BELL, latch_isr);
    irq_set_priority(SIO_IRQ_BELL, PICO_HIGHEST_IRQ_PRIORITY);
    irq_set_enabled(SIO_IRQ_BELL, true);
    core1_ready = true;

#if PICOVDP_DEBUG
    uint32_t thread_events = 0;  // while injecting: the events the thread has latched
#endif
    for (;;) {
#if PICOVDP_DEBUG
        if (request_sequence != request_done) apply_request();
        if (stats_reset_request) reset_stats();

        // An injection takes each latch in the thread (inject.h). The switch
        // either way is made with the latch interrupt held off, so every event
        // is latched exactly once, by one of the two.
        if (!inject_active() && inject_start_requested()) {
            const uint32_t irq = save_and_disable_interrupts();
            inject_set_active(true);
            thread_events = events_latched;
            restore_interrupts(irq);
            scene = -1;
        } else if (inject_active() && inject_stop_requested()) {
            const uint32_t irq = save_and_disable_interrupts();
            if (thread_events == events_latched) inject_set_active(false);
            restore_interrupts(irq);
        }
        if (inject_active()) {
            if (thread_events == events_latched) continue;
            const uint32_t n = thread_events++;
            const line_event_t *e = &events[n & (EVENTS - 1)];
            const uint32_t t0 = cycles();
            inject_before_latch(&card, e->frame, e->screen_line);
            const uint32_t irq = save_and_disable_interrupts();
            vdp_latch(&card, e->screen_line, n);
            restore_interrupts(irq);
            // Latches the interrupt took before the switch come first.
            while (vdp_catch_up(&card)) {
                maximum(&stats.catch_up_max, cycles() - t0);
                render_line(card.render_tag);
            }
            inject_after_line(&card, e->frame, e->screen_line);
            continue;
        }
#endif
        const uint32_t t0 = cycles();
        if (!vdp_catch_up(&card)) continue;
#if PICOVDP_DEBUG
        maximum(&stats.catch_up_max, cycles() - t0);
#else
        (void)t0;
#endif
        const uint32_t n = card.render_tag;
        render_line(n);
#if PICOVDP_DEBUG
        if (events[n & (EVENTS - 1)].screen_line == SCENE_LINE) scene_line();
#endif
    }
}

// ============================================================================
// Setting up

void renderer_init(void) {
    vdp_init(&card, PICOVDP_VERSION_BCD);
    for (unsigned b = 0; b < BUFFERS; b++) memset(buffers[b].indices, 0, VDP_WIDTH);
    bell = multicore_doorbell_claim_unused((1u << 0) | (1u << 1), true);
    vga_init(line_start);
}

void renderer_start(void) {
    cycle_counter_start();
    // PLAN.md section 3: core 1 takes its launch handshake off the FIFO before
    // anything else can use it, and the raster starts only once its latch is
    // in place.
    // Its own stack: the SDK's 2 KB in scratch RAM is no margin.
    static uint32_t core1_stack[2048];
    multicore_launch_core1_with_stack(core1_main, core1_stack, sizeof core1_stack);
    while (!core1_ready) {
    }
    irq_set_exclusive_handler(SIO_IRQ_FIFO, half_job_isr);
    irq_set_priority(SIO_IRQ_FIFO, 0x40);  // under the raster, over USB
    irq_set_enabled(SIO_IRQ_FIFO, true);
    vga_start();
}

static const uint32_t *safe_line(uint16_t screen_line) {
    (void)screen_line;
    return buffers[0].words;
}

void renderer_start_safe(uint16_t bgr) {
    for (unsigned w = 0; w < VGA_RGB_WORDS - 1; w++) buffers[0].words[w] = (uint32_t)bgr * 0x10001u;
    buffers[0].words[VGA_RGB_WORDS - 1] = 0;
    vga_init(safe_line);
    vga_start();
}

uint32_t renderer_heartbeat(void) {
    return heartbeat;
}

#if PICOVDP_DEBUG

// ============================================================================
// Debug: what the link asks for

void renderer_stats(renderer_stats_t *out, bool reset) {
    const stats_raw_t s = stats;  // a copy; fields may move under it, each is whole
    memset(out, 0, sizeof *out);
    out->uptime_us = time_us_64();
    out->clock_hz = clock_get_hz(clk_sys);
    out->budget = vga_display_line_cycles();
    out->rows_built = s.rows_built;
    out->lines_taken = s.lines_taken;
    out->late_rows = s.late_rows;
    out->latches_merged = card.latches_merged - s.merged_base;
    out->missed_bells = s.missed_bells;
    out->journal_overflows = card.journal_overflows - s.overflows_base;
    out->raster_slips = vga_raster_slips() - s.slips_base;
    out->latency.max = s.latency_max;
    out->build.max = s.build_max;
    if (s.rows_built) {
        out->latency.mean = (uint32_t)(s.latency_sum / s.rows_built);
        out->build.mean = (uint32_t)(s.build_sum / s.rows_built);
    }
    // The 99.9th percentile from the histogram, to the top of its bin.
    uint64_t total = 0;
    for (unsigned i = 0; i < RENDERER_HISTOGRAM_BINS; i++) total += s.histogram[i];
    uint64_t below = 0;
    for (unsigned i = 0; i < RENDERER_HISTOGRAM_BINS; i++) {
        below += s.histogram[i];
        if (below * 1000 >= total * 999) {
            out->latency.p999 = ((i + 1) << RENDERER_HISTOGRAM_SHIFT) - 1;
            break;
        }
    }
    out->catch_up_max = s.catch_up_max;
    out->half_max = s.half_max;
    out->wait_max = s.wait_max;
    out->publish_max = s.publish_max;
    out->expand_max = s.expand_max;
    out->core0_half_max = s.core0_max;
    out->split_rows = s.split_rows;
    out->split_mean = s.split_rows ? (uint32_t)(s.split_sum / s.split_rows) : 0;
    out->latch_isr_max = s.latch_isr_max;
    out->line_isr_max = s.line_isr_max;
    out->bus_standins = s.bus_standins;
    memcpy(out->histogram, s.histogram, sizeof out->histogram);
    if (reset) stats_reset_request = true;
}

void renderer_capture_arm(uint32_t frame) {
    capture_state = CAPTURE_IDLE;
    capture_frame = frame;
    capture_state = CAPTURE_ARMED;
}

const renderer_snapshot_t *renderer_capture_wait(uint32_t timeout_ms) {
    const absolute_time_t until = make_timeout_time_ms(timeout_ms);
    while (capture_state != CAPTURE_DONE) {
        if (capture_state == CAPTURE_IDLE || time_reached(until)) return NULL;
        sleep_ms(1);
    }
    // The state as the frame's last row finished: wait for core 1 to get there.
    for (;;) {
        const uint32_t sequence = state_sequence;
        if (!(sequence & 1)) {
            capture->state = state_at_239;
            if (state_sequence == sequence && capture->state.frame >= capture->frame) break;
        }
        if (time_reached(until)) break;
        sleep_ms(1);
    }
    capture->state_matches = capture->state.frame == capture->frame;
    return capture;
}

void renderer_capture_release(void) {
    if (capture_state == CAPTURE_DONE) capture_state = CAPTURE_IDLE;
}

static bool submit(const request_t *r, uint32_t timeout_ms) {
    request = *r;
    const uint32_t sequence = request_sequence + 1;
    request_sequence = sequence;
    const absolute_time_t until = make_timeout_time_ms(timeout_ms);
    while (request_done != sequence) {
        if (time_reached(until)) return false;
        sleep_us(200);
    }
    return true;
}

bool renderer_vram(uint8_t *out, uint32_t timeout_ms) {
    return submit(&(request_t){.kind = REQUEST_VRAM, .vram = out}, timeout_ms);
}

bool renderer_reset(bool power_on, uint32_t timeout_ms) {
    return submit(&(request_t){.kind = REQUEST_RESET, .power_on = power_on}, timeout_ms);
}

bool renderer_load(const renderer_load_t *l, uint32_t timeout_ms) {
    return submit(&(request_t){.kind = REQUEST_LOAD, .load = *l}, timeout_ms);
}

bool renderer_scene(unsigned index, uint32_t timeout_ms) {
    return submit(&(request_t){.kind = REQUEST_SCENE, .scene = index}, timeout_ms);
}

bool renderer_profile(uint16_t row, uint32_t iterations, profile_t *out, uint32_t timeout_ms) {
    return submit(&(request_t){.kind = REQUEST_PROFILE, .row = row, .iterations = iterations, .profile = out}, timeout_ms);
}

void renderer_fault(unsigned kind) {
    submit(&(request_t){.kind = REQUEST_FAULT, .fault = kind}, 1000);
}

unsigned renderer_scene_log(renderer_scene_frame_t *out, unsigned max) {
    unsigned taken = 0;
    while (taken < max && scene_log_tail != scene_log_head) {
        out[taken++] = scene_log[scene_log_tail & 63];
        scene_log_tail++;
    }
    return taken;
}

// ---- on core 1 ----

static void port_write(void *context, unsigned port, uint8_t value) {
    (void)context;
    const uint32_t irq = save_and_disable_interrupts();
    vdp_write(&card, port, value);
    restore_interrupts(irq);
}

static uint8_t port_read(void *context, unsigned port) {
    (void)context;
    const uint32_t irq = save_and_disable_interrupts();
    const uint8_t value = vdp_read(&card, port);
    restore_interrupts(irq);
    return value;
}

static const scene_port_t card_port = {port_write, port_read, NULL};

// The bus stand-in (docs/results/phase-01.md's): a data write on port B into
// the palette window, of the byte already there, as often as the load asks.
// Every 512 writes the pointer is set back to the window's start.
static void __isr __time_critical_func(bus_standin_isr)(void) {
    pwm_hw->intr = 1u << BUS_SLICE;
    const uint16_t base = (uint16_t)((card.reg[0x0c] & 0x3f) << 10);
    if ((bus_writes++ & 511) == 0) {
        vdp_write(&card, 3, (uint8_t)base);
        vdp_write(&card, 3, (uint8_t)(0x40 | ((base >> 8) & 0x3f)));
    }
    vdp_write(&card, 2, card.vram[card.port[1].pointer]);
    stats.bus_standins++;
}

static void bus_standin(uint32_t rate) {
    pwm_set_enabled(BUS_SLICE, false);
    irq_set_enabled(PWM_IRQ_WRAP_0, false);
    pwm_set_irq0_enabled(BUS_SLICE, false);
    pwm_clear_irq(BUS_SLICE);
    if (!rate) return;
    // VBANK for the window, as a program would set it.
    port_write(NULL, 3, (uint8_t)(card.reg[0x0c] >> 4 & 0x03));
    port_write(NULL, 3, 0x88);
    pwm_config config = pwm_get_default_config();
    pwm_config_set_clkdiv_int(&config, 1);
    pwm_config_set_wrap(&config, (uint16_t)(clock_get_hz(clk_sys) / rate - 1));
    pwm_init(BUS_SLICE, &config, false);
    irq_set_exclusive_handler(PWM_IRQ_WRAP_0, bus_standin_isr);
    irq_set_priority(PWM_IRQ_WRAP_0, PICO_HIGHEST_IRQ_PRIORITY);
    pwm_set_irq0_enabled(BUS_SLICE, true);
    irq_set_enabled(PWM_IRQ_WRAP_0, true);
    pwm_set_enabled(BUS_SLICE, true);
}

// A handicapped row's build takes at least the load's cycles: §18's late line, on purpose.
static void pad_build(uint16_t row, uint32_t t0) {
    if (!load.handicap_cycles || row < load.handicap_first || row >= load.handicap_last) return;
    if ((row - load.handicap_first) % (load.handicap_every ? load.handicap_every : 1)) return;
    while (cycles() - t0 < load.handicap_cycles) {
    }
}

// The scene's program, at screen line 250.
static void scene_line(void) {
    if (scene_pending) {
        scene_pending = false;
        uint32_t irq = save_and_disable_interrupts();
        vdp_reset(&card, true);
        restore_interrupts(irq);
        scene_setup(scene_at((unsigned)scene), &card_port);
        scene_frame_count = 0;
        return;
    }
    if (scene < 0) return;
    renderer_scene_frame_t *f = &scene_log[scene_log_head & 63];
    f->scene_frame = scene_frame_count;
    scene_reads(&card_port, f->reads);
    if (scene_log_head - scene_log_tail < 64) scene_log_head++;
    scene_frame(scene_at((unsigned)scene), ++scene_frame_count, &card_port);
    if (load.fonts) scene_fonts(scene_at((unsigned)scene), &card_port);
}

static void apply_request(void) {
    const uint32_t sequence = request_sequence;
    uint32_t irq;
    switch (request.kind) {
    case REQUEST_RESET:
        irq = save_and_disable_interrupts();
        vdp_reset(&card, request.power_on);
        restore_interrupts(irq);
        scene = -1;
        break;
    case REQUEST_VRAM:
        irq = save_and_disable_interrupts();
        memcpy(request.vram, card.vram, VDP_VRAM_SIZE);
        restore_interrupts(irq);
        break;
    case REQUEST_SCENE:
        scene = (int)request.scene;
        scene_pending = true;  // at the next screen line 250
        scene_log_tail = scene_log_head;
        break;
    case REQUEST_LOAD:
        load = request.load;
        bus_standin(load.bus_rate_hz);
        break;
    case REQUEST_FAULT:
        request_done = sequence;
        fault_raise(request.fault);
        break;
    case REQUEST_PROFILE:
        profile_row(&card, request.row, request.iterations, request.profile);
        break;
    }
    request_done = sequence;
}

static void reset_stats(void) {
    stats_reset_request = false;
    memset(&stats, 0, sizeof stats);
    stats.merged_base = card.latches_merged;
    stats.overflows_base = card.journal_overflows;
    stats.slips_base = vga_raster_slips();
}

#endif
