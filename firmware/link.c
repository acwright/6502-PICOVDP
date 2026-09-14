// The debug link. See link.h and docs/DEBUGLINK.md.

#include "link.h"

#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/stdio.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"

#include "fault.h"
#include "inject.h"
#include "profile.h"
#include "renderer.h"
#include "scenes.h"

#define PROTOCOL 1
#define SYNC0 'P'
#define SYNC1 'V'
#define HEADER 8
#define MAX_REQUEST 16384

enum {
    CMD_INFO = 0x01,
    CMD_STATS = 0x02,
    CMD_SNAPSHOT = 0x03,
    CMD_VRAM = 0x04,
    CMD_INJECT = 0x05,
    CMD_RESET = 0x06,
    CMD_REBOOT = 0x07,
    CMD_FAULT = 0x08,
    CMD_SCENE = 0x09,
    CMD_LOAD = 0x0a,
    CMD_SCENE_LOG = 0x0b,
    CMD_PROFILE = 0x0c,
    RESPONSE = 0x80,
    LOG = 0x7f,
    ERROR = 0xff,
};

// ---- sending ----

typedef struct writer {
    uint8_t *at;
    uint8_t *end;
} writer_t;

static void put(writer_t *w, const void *bytes, uint32_t count) {
    if ((uint32_t)(w->end - w->at) < count) count = (uint32_t)(w->end - w->at);
    memcpy(w->at, bytes, count);
    w->at += count;
}
static void put8(writer_t *w, uint8_t v) { put(w, &v, 1); }
static void put16(writer_t *w, uint16_t v) { put(w, (uint8_t[]){(uint8_t)v, (uint8_t)(v >> 8)}, 2); }
static void put32(writer_t *w, uint32_t v) { put16(w, (uint16_t)v); put16(w, (uint16_t)(v >> 16)); }
static void put64(writer_t *w, uint64_t v) { put32(w, (uint32_t)v); put32(w, (uint32_t)(v >> 32)); }
static void put_text(writer_t *w, const char *text, uint32_t size) {
    char field[64] = {0};
    snprintf(field, sizeof field, "%s", text);
    put(w, field, size);
}

static void out(const uint8_t *bytes, uint32_t count) {
    while (count) {
        const uint32_t chunk = count > 4096 ? 4096 : count;
        stdio_put_string((const char *)bytes, (int)chunk, false, false);
        bytes += chunk;
        count -= chunk;
    }
}

// A packet of up to two parts.
static void send(uint8_t type, uint8_t sequence, const uint8_t *a, uint32_t a_count, const uint8_t *b, uint32_t b_count) {
    const uint32_t length = a_count + b_count;
    uint8_t header[HEADER] = {SYNC0, SYNC1, type, sequence, (uint8_t)length, (uint8_t)(length >> 8),
                              (uint8_t)(length >> 16), (uint8_t)(length >> 24)};
    uint32_t crc = fault_crc32(0, header + 2, HEADER - 2);
    if (a_count) crc = fault_crc32(crc, a, a_count);
    if (b_count) crc = fault_crc32(crc, b, b_count);
    const uint8_t trailer[4] = {(uint8_t)crc, (uint8_t)(crc >> 8), (uint8_t)(crc >> 16), (uint8_t)(crc >> 24)};
    out(header, HEADER);
    if (a_count) out(a, a_count);
    if (b_count) out(b, b_count);
    out(trailer, 4);
    stdio_flush();
}

static void error(uint8_t sequence, const char *message) {
    send(ERROR, sequence, (const uint8_t *)message, (uint32_t)strlen(message), NULL, 0);
}

static uint8_t response[4096];

static void reply(uint8_t command, uint8_t sequence, const writer_t *w) {
    send((uint8_t)(RESPONSE | command), sequence, response, (uint32_t)(w->at - response), NULL, 0);
}

// ---- the commands ----

static void put_record(writer_t *w, const fault_record_t *r) {
    put32(w, r->kind);
    put32(w, r->core);
    const uint32_t registers[] = {r->r0, r->r1, r->r2, r->r3, r->r12, r->lr, r->pc, r->xpsr,
                                  r->exc_return, r->sp, r->cfsr, r->hfsr, r->mmfar, r->bfar, r->sfsr, r->sfar};
    for (unsigned i = 0; i < sizeof registers / sizeof registers[0]; i++) put32(w, registers[i]);
    for (unsigned i = 0; i < FAULT_STACK_WORDS; i++) put32(w, r->stack[i]);
    put64(w, r->uptime_us);
    put32(w, r->heartbeat_core0);
    put32(w, r->heartbeat_core1);
    put(w, r->message, sizeof r->message);
    put(w, r->build, sizeof r->build);
}

static reset_reason_t boot_reason;

static void info(uint8_t sequence, bool safe_mode) {
    writer_t w = {response, response + sizeof response};
    char id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    pico_get_unique_board_id_string(id, sizeof id);
    put8(&w, PROTOCOL);
    put8(&w, safe_mode);
    put8(&w, (uint8_t)boot_reason);
    put8(&w, PICOVDP_VERSION_BCD);
    put32(&w, clock_get_hz(clk_sys));
    put64(&w, time_us_64());
    put_text(&w, PICOVDP_BUILD_ID, 32);
    put_text(&w, PICO_BOARD, 16);
    put_text(&w, id, 24);
    fault_record_t record;
    const bool present = fault_last(&record);
    put8(&w, present);
    if (present) put_record(&w, &record);
    reply(CMD_INFO, sequence, &w);
}

#if PICOVDP_DEBUG

static void put_timing(writer_t *w, const renderer_timing_t *t) {
    put32(w, t->max);
    put32(w, t->p999);
    put32(w, t->mean);
}

static void stats(uint8_t sequence, const uint8_t *payload, uint32_t count) {
    static renderer_stats_t s;
    renderer_stats(&s, count >= 1 && (payload[0] & 1));
    writer_t w = {response, response + sizeof response};
    put64(&w, s.uptime_us);
    put32(&w, s.clock_hz);
    put32(&w, s.budget);
    put32(&w, s.rows_built);
    put32(&w, s.lines_taken);
    put32(&w, s.late_rows);
    put32(&w, s.latches_merged);
    put32(&w, s.missed_bells);
    put32(&w, s.journal_overflows);
    put32(&w, s.raster_slips);
    put_timing(&w, &s.latency);
    put_timing(&w, &s.build);
    const uint32_t stages[] = {s.catch_up_max, s.half_max, s.expand_max, s.wait_max, s.publish_max, s.core0_half_max};
    for (unsigned i = 0; i < sizeof stages / sizeof stages[0]; i++) put32(&w, stages[i]);
    put32(&w, s.split_mean);
    put32(&w, s.split_rows);
    put32(&w, s.latch_isr_max);
    put32(&w, s.line_isr_max);
    put32(&w, s.bus_standins);
    put16(&w, RENDERER_HISTOGRAM_BINS);
    put8(&w, RENDERER_HISTOGRAM_SHIFT);
    for (unsigned i = 0; i < RENDERER_HISTOGRAM_BINS; i++) put16(&w, s.histogram[i]);
    reply(CMD_STATS, sequence, &w);
}

static void put_state(writer_t *w, const renderer_state_t *s) {
    put(w, s->card.registers, VDP_REGISTERS);
    for (unsigned pair = 0; pair < 2; pair++) {
        const vdp_port_t *p = &s->card.port[pair];
        put16(w, p->pointer);
        put8(w, p->prefetch);
        put8(w, p->payload);
        put8(w, p->read_mode);
        put8(w, p->second);
    }
    put16(w, s->card.screen_line);
    put16(w, s->card.display_line);
    put8(w, s->card.stat0);
    put8(w, s->card.irq_latch);
    put8(w, s->card.frame_events);
    put8(w, s->card.overflow_sprite);
    put(w, s->card.collision_map, 8);
    put8(w, s->interrupt);
    put32(w, s->frame);
    put32(w, s->scene_frame);
}

static void snapshot(uint8_t sequence, const uint8_t *payload, uint32_t count) {
    if (count < 9) return error(sequence, "SNAPSHOT: mode, frame, timeout");
    const uint8_t mode = payload[0];
    const uint32_t frame = payload[1] | payload[2] << 8 | payload[3] << 16 | (uint32_t)payload[4] << 24;
    const uint32_t timeout = payload[5] | payload[6] << 8 | payload[7] << 16 | (uint32_t)payload[8] << 24;
    if (mode != 2) renderer_capture_arm(mode == 1 ? frame : 0xffffffffu);
    const renderer_snapshot_t *s = renderer_capture_wait(timeout);
    if (!s) return error(sequence, "SNAPSHOT: timed out");
    writer_t w = {response, response + sizeof response};
    put32(&w, s->frame);
    put8(&w, s->state_matches);
    put_state(&w, &s->state);
    put(&w, s->source, 240);
    put(&w, s->late, 240);
    send(RESPONSE | CMD_SNAPSHOT, sequence, response, (uint32_t)(w.at - response), s->rows, sizeof s->rows);
    renderer_capture_release();
}

static uint8_t vram_copy[VDP_VRAM_SIZE];

static void vram(uint8_t sequence) {
    if (!renderer_vram(vram_copy, 1000)) return error(sequence, "VRAM: core 1 did not answer");
    send(RESPONSE | CMD_VRAM, sequence, vram_copy, sizeof vram_copy, NULL, 0);
}

static void inject(uint8_t sequence, const uint8_t *payload, uint32_t count) {
    if (count < 1) return error(sequence, "INJECT: no subcommand");
    uint32_t taken = 0;
    switch (payload[0]) {
    case 0:  // BEGIN
        if (count < 3) return error(sequence, "INJECT BEGIN: capture frame");
        if (!inject_begin((uint16_t)(payload[1] | payload[2] << 8))) return error(sequence, "INJECT BEGIN: one is running");
        break;
    case 1:  // DATA
        taken = inject_data(payload + 1, count - 1);
        break;
    case 2:  // STATUS
        break;
    case 3:  // ABORT
        inject_abort();
        break;
    default:
        return error(sequence, "INJECT: unknown subcommand");
    }
    static inject_status_t s;
    inject_status(&s);
    writer_t w = {response, response + sizeof response};
    put8(&w, s.state);
    put32(&w, s.base_frame);
    put16(&w, s.frame);
    put16(&w, s.screen_line);
    put32(&w, s.ops);
    put32(&w, s.reads);
    put32(&w, s.stat5_reads);
    put32(&w, s.mismatches);
    put32(&w, s.space);
    put32(&w, s.buffered);
    put32(&w, taken);
    put_state(&w, &s.end_state);
    put8(&w, s.logged);
    for (unsigned i = 0; i < s.logged; i++) {
        const inject_mismatch_t *m = &s.log[i];
        put32(&w, m->op);
        put16(&w, m->frame);
        put16(&w, m->screen_line);
        put8(&w, m->kind);
        put8(&w, m->port);
        put8(&w, m->expected);
        put8(&w, m->got);
        put8(&w, m->select);
    }
    reply(CMD_INJECT, sequence, &w);
}

static void scene(uint8_t sequence, const uint8_t *payload, uint32_t count) {
    char name[33] = {0};
    memcpy(name, payload, count < 32 ? count : 32);
    const int index = scene_find(name);
    if (index < 0) return error(sequence, "SCENE: no such scene");
    if (!renderer_scene((unsigned)index, 1000)) return error(sequence, "SCENE: core 1 did not answer");
    writer_t w = {response, response + sizeof response};
    put8(&w, (uint8_t)index);
    put_text(&w, scene_at((unsigned)index)->name, 32);
    reply(CMD_SCENE, sequence, &w);
}

static void load(uint8_t sequence, const uint8_t *payload, uint32_t count) {
    if (count < 14) return error(sequence, "LOAD: bus rate, handicap first, last, every, cycles");
    renderer_load_t l = {
        .bus_rate_hz = payload[0] | payload[1] << 8 | payload[2] << 16 | (uint32_t)payload[3] << 24,
        .handicap_first = (uint16_t)(payload[4] | payload[5] << 8),
        .handicap_last = (uint16_t)(payload[6] | payload[7] << 8),
        .handicap_every = (uint16_t)(payload[8] | payload[9] << 8),
        .handicap_cycles = payload[10] | payload[11] << 8 | payload[12] << 16 | (uint32_t)payload[13] << 24,
    };
    if (!renderer_load(&l, 1000)) return error(sequence, "LOAD: core 1 did not answer");
    writer_t w = {response, response + sizeof response};
    reply(CMD_LOAD, sequence, &w);
}

static void scene_log(uint8_t sequence) {
    static renderer_scene_frame_t frames[64];
    const unsigned taken = renderer_scene_log(frames, 64);
    writer_t w = {response, response + sizeof response};
    put8(&w, (uint8_t)taken);
    put8(&w, SCENE_READS);
    for (unsigned i = 0; i < taken; i++) {
        put32(&w, frames[i].scene_frame);
        put(&w, frames[i].reads, SCENE_READS);
    }
    reply(CMD_SCENE_LOG, sequence, &w);
}

static void profile(uint8_t sequence, const uint8_t *payload, uint32_t count) {
    if (count < 6) return error(sequence, "PROFILE: row, iterations");
    const uint16_t row = (uint16_t)(payload[0] | payload[1] << 8);
    const uint32_t iterations = payload[2] | payload[3] << 8 | payload[4] << 16 | (uint32_t)payload[5] << 24;
    static profile_t p;
    if (!renderer_profile(row, iterations, &p, 30000)) return error(sequence, "PROFILE: core 1 did not answer");
    writer_t w = {response, response + sizeof response};
    put16(&w, p.row);
    put8(&w, p.sprites);
    put32(&w, p.probe);
    put8(&w, PROFILE_STAGES);
    for (unsigned i = 0; i < PROFILE_STAGES; i++) {
        put32(&w, p.min[i]);
        put32(&w, p.max[i]);
    }
    reply(CMD_PROFILE, sequence, &w);
}

#endif

static void dispatch(uint8_t type, uint8_t sequence, const uint8_t *payload, uint32_t count, bool safe_mode) {
    switch (type) {
    case CMD_INFO:
        return info(sequence, safe_mode);
    case CMD_REBOOT: {
        writer_t w = {response, response + sizeof response};
        reply(CMD_REBOOT, sequence, &w);
        sleep_ms(50);
        fault_clear();
        if (count >= 1 && payload[0]) rom_reset_usb_boot(0, 0);
        watchdog_reboot(0, 0, 0);
        for (;;) {
        }
    }
    case CMD_FAULT: {
        if (count < 1) return error(sequence, "FAULT: kind");
        writer_t w = {response, response + sizeof response};
        reply(CMD_FAULT, sequence, &w);
        sleep_ms(50);
#if PICOVDP_DEBUG
        if (payload[0] == FAULT_RAISE_CORE1_HARDFAULT || payload[0] == FAULT_RAISE_CORE1_HANG) {
            if (!safe_mode) renderer_fault(payload[0]);
            return;
        }
#endif
        fault_raise(payload[0]);
        return;
    }
    }
#if PICOVDP_DEBUG
    if (safe_mode) return error(sequence, "safe mode: INFO, REBOOT and FAULT only");
    switch (type) {
    case CMD_STATS:
        return stats(sequence, payload, count);
    case CMD_SNAPSHOT:
        return snapshot(sequence, payload, count);
    case CMD_VRAM:
        return vram(sequence);
    case CMD_INJECT:
        return inject(sequence, payload, count);
    case CMD_RESET: {
        if (!renderer_reset(count >= 1 && payload[0], 1000)) return error(sequence, "RESET: core 1 did not answer");
        writer_t w = {response, response + sizeof response};
        return reply(CMD_RESET, sequence, &w);
    }
    case CMD_SCENE:
        return scene(sequence, payload, count);
    case CMD_LOAD:
        return load(sequence, payload, count);
    case CMD_SCENE_LOG:
        return scene_log(sequence);
    case CMD_PROFILE:
        return profile(sequence, payload, count);
    }
#else
    (void)payload;
#endif
    error(sequence, "unknown command");
}

// ---- receiving ----

static uint8_t packet[HEADER + MAX_REQUEST + 4];

void link_run(bool safe_mode) {
    boot_reason = fault_boot_reason();
    stdio_set_translate_crlf(&stdio_usb, false);
    uint32_t have = 0;
    uint32_t need = HEADER;
    static char chunk[512];
    for (;;) {
        fault_thread_alive();
        const int got = stdio_get_until(chunk, sizeof chunk, make_timeout_time_us(2000));
        if (got <= 0) continue;
        for (int i = 0; i < got; i++) {
            const uint8_t byte = (uint8_t)chunk[i];
            // Hunt for the sync bytes.
            if (have == 0 && byte != SYNC0) continue;
            if (have == 1 && byte != SYNC1) {
                have = byte == SYNC0 ? 1 : 0;
                continue;
            }
            packet[have++] = byte;
            if (have == HEADER) {
                const uint32_t length = packet[4] | packet[5] << 8 | packet[6] << 16 | (uint32_t)packet[7] << 24;
                if (length > MAX_REQUEST) {
                    have = 0;
                    continue;
                }
                need = HEADER + length + 4;
            }
            if (have < HEADER || have < need) continue;
            const uint32_t length = need - HEADER - 4;
            const uint32_t crc = fault_crc32(0, packet + 2, HEADER - 2 + length);
            const uint8_t *t = packet + HEADER + length;
            const uint32_t sent = t[0] | t[1] << 8 | t[2] << 16 | (uint32_t)t[3] << 24;
            if (crc == sent) dispatch(packet[2], packet[3], packet + HEADER, length, safe_mode);
            else error(packet[3], "bad CRC");
            have = 0;
            need = HEADER;
        }
    }
}
