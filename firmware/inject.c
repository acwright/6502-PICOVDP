// The injection executor. See inject.h.

#include "inject.h"

#include <string.h>

#include "hardware/sync.h"

#define RING 32768                       // a power of two
static uint8_t ring[RING];
static volatile uint32_t ring_in, ring_out;  // core 0 writes ring_in, core 1 ring_out

static volatile uint8_t state = INJECT_IDLE;
static volatile bool active, start_requested, stop_requested;
static uint16_t capture_relative;

static uint32_t base_frame;
static uint16_t line_frame, line_screen;   // the LINE record in force
static uint32_t ops, reads, stat5_reads, mismatches;
static renderer_state_t end_state;
static inject_mismatch_t log_entries[INJECT_LOG];
static uint8_t logged;

// ---- core 0 ----

bool inject_begin(uint16_t capture_frame) {
    if (active || start_requested) return false;
    ring_in = ring_out = 0;
    ops = reads = stat5_reads = mismatches = 0;
    logged = 0;
    capture_relative = capture_frame;
    state = INJECT_ARMED;
    stop_requested = false;
    start_requested = true;
    return true;
}

uint32_t inject_data(const uint8_t *bytes, uint32_t count) {
    const uint32_t space = RING - (ring_in - ring_out);
    if (count > space) count = space;
    uint32_t in = ring_in;
    for (uint32_t i = 0; i < count; i++) ring[(in + i) & (RING - 1)] = bytes[i];
    ring_in = in + count;
    return count;
}

void inject_abort(void) {
    start_requested = false;
    stop_requested = true;
}

void inject_status(inject_status_t *out) {
    memset(out, 0, sizeof *out);
    out->state = state;
    out->base_frame = base_frame;
    out->frame = line_frame;
    out->screen_line = line_screen;
    out->ops = ops;
    out->reads = reads;
    out->stat5_reads = stat5_reads;
    out->mismatches = mismatches;
    out->buffered = ring_in - ring_out;
    out->space = RING - out->buffered;
    if (state == INJECT_ENDED) out->end_state = end_state;
    out->logged = logged;
    memcpy(out->log, log_entries, sizeof log_entries);
}

// ---- core 1 ----

bool inject_active(void) {
    return active;
}

bool inject_start_requested(void) {
    return start_requested;
}

bool inject_stop_requested(void) {
    return stop_requested;
}

void inject_set_active(bool on) {
    active = on;
    if (on) {
        start_requested = false;
    } else {
        stop_requested = false;
        if (state != INJECT_ENDED && state != INJECT_FAILED) state = INJECT_IDLE;
    }
}

static inline uint32_t buffered(void) {
    return ring_in - ring_out;
}

static inline uint8_t peek(uint32_t offset) {
    return ring[(ring_out + offset) & (RING - 1)];
}

static void mismatch(uint8_t kind, uint8_t port, uint8_t expected, uint8_t got, uint8_t select) {
    mismatches++;
    if (logged < INJECT_LOG) {
        log_entries[logged++] = (inject_mismatch_t){ops, line_frame, line_screen, kind, port, expected, got, select};
    }
}

static void fail(uint8_t kind) {
    mismatch(kind, 0, 0, 0, 0);
    state = INJECT_FAILED;
}

void inject_before_latch(vdp_t *v, uint32_t frame, uint16_t screen_line) {
    if (state != INJECT_ARMED || buffered() < 3) return;
    if (peek(0) != INJECT_COLD) {
        fail(MISMATCH_BAD_RECORD);
        return;
    }
    if (screen_line != (uint16_t)(peek(1) | peek(2) << 8)) return;
    ring_out += 3;
    // TRACE.md's X cold: a power-on reset, the raster at this line, and its
    // line start, which is the latch the thread takes next.
    const uint32_t irq = save_and_disable_interrupts();
    vdp_reset(v, true);
    restore_interrupts(irq);
    base_frame = frame;
    line_frame = 0;
    line_screen = screen_line;
    state = INJECT_RUNNING;
    if (capture_relative != 0xffff) renderer_capture_arm(frame + capture_relative);
}

void inject_after_line(vdp_t *v, uint32_t frame, uint16_t screen_line) {
    if (state != INJECT_RUNNING) return;
    const uint32_t now = ((frame - base_frame) << 9) | screen_line;
    for (;;) {
        const uint32_t available = buffered();
        if (!available) return;
        const uint8_t type = peek(0);
        if (type & INJECT_OP) {
            if (available < 2) return;
            const uint8_t value = peek(1);
            ring_out += 2;
            const unsigned port = type & 3;
            const bool interrupt = (type & 0x20) != 0;
            const uint32_t irq = save_and_disable_interrupts();
            if (type & 0x40) {
                // §6: which status a status port reads, before the read.
                const uint8_t select = (port & 1) ? v->reg[port & 2 ? 0x0e : 0x0f] & 0x0f : 0xff;
                const uint8_t got = vdp_read(v, port);
                const bool asserted = vdp_int_asserted(v);
                restore_interrupts(irq);
                reads++;
                // Not compared on silicon (PLAN.md section 4): STAT5 is the
                // firmware's version, STAT3 b1 is advisory.
                if (select == 5) {
                    stat5_reads++;
                } else if ((got ^ value) & (select == 3 ? 0xfd : 0xff)) {
                    mismatch(MISMATCH_READ, (uint8_t)port, value, got, select);
                }
                if (asserted != interrupt) mismatch(MISMATCH_INT_AFTER_OP, (uint8_t)port, interrupt, asserted, select);
            } else {
                vdp_write(v, port, value);
                const bool asserted = vdp_int_asserted(v);
                restore_interrupts(irq);
                if (asserted != interrupt) mismatch(MISMATCH_INT_AFTER_OP, (uint8_t)port, interrupt, asserted, 0xff);
            }
            ops++;
        } else if (type == INJECT_LINE) {
            if (available < 6) return;
            const uint16_t f = (uint16_t)(peek(1) | peek(2) << 8), s = (uint16_t)(peek(3) | peek(4) << 8);
            const uint32_t at = ((uint32_t)f << 9) | s;
            if (at > now) return;  // a later line's
            const uint8_t interrupt = peek(5);
            ring_out += 6;
            line_frame = f;
            line_screen = s;
            if (at < now) {
                // The host streamed it after its line had passed.
                fail(MISMATCH_LATE_RECORD);
                return;
            }
            const bool asserted = vdp_int_asserted(v);
            if (interrupt != 0xff && asserted != (interrupt != 0)) {
                mismatch(MISMATCH_INT_AFTER_LATCH, 0, interrupt, asserted, 0xff);
            }
        } else if (type == INJECT_END) {
            ring_out += 1;
            const uint32_t irq = save_and_disable_interrupts();
            vdp_snapshot_t *s = &end_state.card;
            memcpy(s->registers, v->reg, sizeof s->registers);
            s->port[0] = v->port[0];
            s->port[1] = v->port[1];
            s->screen_line = v->screen_line;
            s->display_line = v->display_line;
            s->stat0 = v->stat0;
            s->irq_latch = v->irq_latch;
            s->frame_events = v->frame_events;
            s->overflow_sprite = v->overflow_sprite;
            memcpy(s->collision_map, v->collision_map, sizeof s->collision_map);
            s->font_pending = v->font_pending;
            memcpy(s->font_id, v->font_id, sizeof s->font_id);
            memcpy(s->font_base, v->font_base, sizeof s->font_base);
            end_state.interrupt = vdp_int_asserted(v);
            restore_interrupts(irq);
            end_state.frame = frame;
            state = INJECT_ENDED;
            return;
        } else {
            fail(MISMATCH_BAD_RECORD);
            return;
        }
    }
}
