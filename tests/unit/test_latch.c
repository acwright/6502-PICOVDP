// §3's latch: a line is built from the card as it stood when the line before
// it began, however the bus writes during the build (PLAN.md section 3). Jest
// cannot see this — the adapter builds each line the instant its latch falls —
// so it is tested here, inside the struct.

#include <string.h>

#include "card.h"
#include "test.h"

// What §11's cache holds for a 12-bit $RGB: 0x0BGR, paired.
static uint32_t cached(uint16_t rgb) {
    uint32_t bgr = (uint32_t)((rgb >> 8) & 0xf) | (rgb & 0x0f0u) | ((uint32_t)(rgb & 0xf) << 8);
    return bgr * 0x10001u;
}

// The render side agrees with the bus side, as it must straight after a latch:
// VRAM, registers, and a cache that is the bus window decoded.
static bool render_in_step(const vdp_t *v) {
    if (memcmp(v->render_vram, v->vram, sizeof v->vram) != 0) return false;
    if (memcmp(v->render_reg, v->reg, sizeof v->reg) != 0) return false;
    for (unsigned entry = 0; entry < VDP_PALETTE_ENTRIES; entry++) {
        if (v->palette[entry] != cached(vdp_debug_palette(v, entry))) return false;
    }
    return true;
}

static void write_entry(vdp_t *v, unsigned entry, uint16_t rgb) {
    uint8_t bytes[] = {(uint8_t)((rgb >> 8) & 0xf), (uint8_t)rgb};
    uint16_t base = (uint16_t)((vdp_debug_register(v, 0x0c) & 0x3f) << 10);
    store(v, (uint16_t)(base + 2 * entry), bytes, 2);
}

// The one PLAN.md names: an operation between vdp_line_start and vdp_build_line
// does not reach that line — not the backdrop register, not the palette entry
// it names, not VRAM.
TEST(operations_after_the_latch_wait_for_the_next) {
    vdp_t *v = new_card();
    uint8_t line[VDP_WIDTH];
    uint16_t rgb[2 * VDP_WIDTH];

    set_reg(v, 0x07, 0x0f);  // backdrop: entry 15, white
    vdp_line_start(v, 10);

    // The bus, mid-build: a new backdrop, a new colour for the old one, VRAM.
    set_reg(v, 0x07, 0x04);
    write_entry(v, 15, 0xf00);
    uint8_t byte = 0x5a;
    store(v, 0x1234, &byte, 1);
    CHECK_EQ(0xf00, vdp_debug_palette(v, 15));  // the bus side has them all at once
    CHECK_EQ(0x5a, vdp_debug_vram(v, 0x1234));

    vdp_build_line(v, line);
    vdp_expand_line(v, line, rgb);
    CHECK_EQ(15, line[0]);
    CHECK_EQ(15, line[VDP_WIDTH - 1]);
    CHECK_EQ(0x0fff, rgb[0]);  // still white
    CHECK_EQ(0x0fff, rgb[2 * VDP_WIDTH - 1]);
    CHECK_EQ(0x00, v->render_vram[0x1234]);

    vdp_line_start(v, 11);
    vdp_build_line(v, line);
    vdp_expand_line(v, line, rgb);
    CHECK_EQ(4, line[0]);
    CHECK_EQ(0x0e55, rgb[0]);  // entry 4, $55E, as 0x0BGR
    CHECK_EQ(0x5a, v->render_vram[0x1234]);
    CHECK(render_in_step(v));

    // And the old backdrop's new colour is in the cache, ready for when it is named.
    set_reg(v, 0x07, 0x0f);
    vdp_line_start(v, 12);
    vdp_build_line(v, line);
    vdp_expand_line(v, line, rgb);
    CHECK_EQ(0x000f, rgb[1]);  // $F00 is red: 0x0BGR 0x00F
    free(v);
}

// Past VDP_JOURNAL_ENTRIES writes in a line, pages are marked and copied whole.
// The render side still ends up exact, the palette window included, and the
// fallback is counted once per latch that used it.
TEST(journal_overflow_copies_pages) {
    vdp_t *v = new_card();
    vdp_line_start(v, 0);

    // A burst no 6502 can make: 1,500 bytes across pages 0 and 1, then the
    // palette window, then one byte into page 63.
    point_at(v, 0, 0x0300, true);
    for (unsigned i = 0; i < 1500; i++) vdp_write(v, 0, (uint8_t)(i * 13));
    write_entry(v, 0x21, 0x9ab);
    uint8_t byte = 0x77;
    store(v, 0xfff0, &byte, 1);
    CHECK_EQ(VDP_JOURNAL_ENTRIES, v->journal_tail - v->journal_head);
    CHECK(v->dirty_pages != 0);

    vdp_line_start(v, 1);
    CHECK(render_in_step(v));
    CHECK_EQ(0x9ab, (uint16_t)(((v->render_vram[0xfc42] & 0xf) << 8) | v->render_vram[0xfc43]));
    CHECK_EQ(cached(0x9ab), v->palette[0x21]);
    CHECK_EQ(1, vdp_debug_stats(v).journal_overflows);
    CHECK_EQ(0, v->journal_tail - v->journal_head);
    CHECK_EQ(0, (long long)v->dirty_pages);

    // A line within the journal does not count.
    store(v, 0x4000, &byte, 1);
    vdp_line_start(v, 2);
    CHECK(render_in_step(v));
    CHECK_EQ(1, vdp_debug_stats(v).journal_overflows);
    free(v);
}

// §11: PALBASE moving during a line — away and back, with writes to both windows
// in between — leaves the cache as the window it ends on, decoded.
TEST(palbase_moves_within_a_line) {
    vdp_t *v = new_card();
    vdp_line_start(v, 0);

    set_reg(v, 0x0c, 0x3c);           // $F000
    write_entry(v, 3, 0x123);         // into $F000's window
    set_reg(v, 0x0c, 0x3f);           // back to $FC00
    write_entry(v, 5, 0x456);         // into $FC00's window
    vdp_line_start(v, 1);
    CHECK(render_in_step(v));
    CHECK_EQ(cached(0x456), v->palette[5]);
    CHECK_EQ(cached(0x6d7), v->palette[3]);  // $FC00's entry 3, untouched

    set_reg(v, 0x0c, 0x3c);
    vdp_line_start(v, 2);
    CHECK(render_in_step(v));
    CHECK_EQ(cached(0x123), v->palette[3]);

    // Bytes either side of the window are not palette.
    uint8_t byte = 0xff;
    store(v, 0xefff, &byte, 1);
    store(v, 0xf200, &byte, 1);
    vdp_line_start(v, 3);
    CHECK(render_in_step(v));
    free(v);
}

// An LCG, the same sequence on every compiler.
static uint32_t random_state = 12345;
static uint32_t random_next(void) {
    random_state = random_state * 1664525u + 1013904223u;
    return random_state >> 8;
}

// Random traffic — commands, data, debugger pokes, bursts past the journal, warm
// resets — with the render side checked against the bus side after every latch.
TEST(random_traffic_keeps_the_render_side_in_step) {
    vdp_t *v = new_card();
    static const uint8_t registers[] = {0x07, 0x08, 0x09, 0x0c, 0x0c, 0x16, 0x01, 0x0a, 0x0d};
    unsigned latches = 0, checked_overflows = 0;

    for (unsigned op = 0; op < 400000; op++) {
        uint32_t roll = random_next() % 1000;
        unsigned pair = random_next() & 1;
        if (roll < 150) {
            uint8_t reg = registers[random_next() % sizeof registers];
            // PALBASE among the top 8 KB, where the pokes below land.
            uint8_t value = reg == 0x0c ? (uint8_t)(0x38 + random_next() % 8) : (uint8_t)random_next();
            command(v, pair, value, (uint8_t)(0x80 | reg));
        } else if (roll < 250) {
            bool high = random_next() & 1;
            uint16_t address = high ? (uint16_t)(0xe000 + random_next() % 0x2000) : (uint16_t)random_next();
            command(v, pair, (uint8_t)address, (uint8_t)(0x40 | ((address >> 8) & 0x3f)));
        } else if (roll < 700) {
            vdp_write(v, 2 * pair, (uint8_t)random_next());
        } else if (roll < 800) {
            unsigned status = random_next() & 1;
            vdp_read(v, 2 * pair + status);
        } else if (roll < 850) {
            uint16_t address = (uint16_t)(0xf000 + random_next() % 0x1000);
            vdp_debug_set_vram(v, address, (uint8_t)random_next());
        } else if (roll < 852) {
            unsigned count = 900 + random_next() % 1200;
            for (unsigned i = 0; i < count; i++) vdp_write(v, 2 * pair, (uint8_t)random_next());
        } else if (roll < 853) {
            vdp_reset(v, false);
        } else {
            vdp_line_start(v, (uint16_t)(latches % VDP_SCREEN_LINES));
            latches++;
            if (!render_in_step(v)) {
                CHECK(render_in_step(v));
                printf("  out of step after operation %u\n", op);
                break;
            }
            checked_overflows = vdp_debug_stats(v).journal_overflows;
        }
    }
    CHECK(latches > 50000);
    CHECK(checked_overflows > 100);
    printf("  %u latches, %u of them past the journal\n", latches, checked_overflows);
    free(v);
}

// The firmware's latch in halves (§18's late lines): a render side that catches
// up after the bus has moved on still takes each line from its own latch — the
// registers, VRAM and palette as they stood then, and the line's number.
TEST(a_late_catch_up_takes_the_card_as_it_stood) {
    vdp_t *v = new_card();
    set_reg(v, 0x07, 0x01);
    uint8_t byte = 0x11;
    store(v, 0x2000, &byte, 1);
    write_entry(v, 7, 0x100);
    vdp_latch(v, 40, 1000);

    set_reg(v, 0x07, 0x02);
    byte = 0x22;
    store(v, 0x2000, &byte, 1);
    write_entry(v, 7, 0x200);
    vdp_latch(v, 41, 1001);

    set_reg(v, 0x07, 0x03);
    byte = 0x33;
    store(v, 0x2000, &byte, 1);
    write_entry(v, 7, 0x300);
    CHECK_EQ(41, v->screen_line);  // the bus side is at the latest latch

    CHECK(vdp_catch_up(v));
    CHECK_EQ(41, v->render_screen_line);
    CHECK_EQ(1000, v->render_tag);
    CHECK_EQ(0x01, v->render_reg[0x07]);
    CHECK_EQ(0x11, v->render_vram[0x2000]);
    CHECK_EQ(cached(0x100), v->palette[7]);

    CHECK(vdp_catch_up(v));
    CHECK_EQ(42, v->render_screen_line);
    CHECK_EQ(1001, v->render_tag);
    CHECK_EQ(0x02, v->render_reg[0x07]);
    CHECK_EQ(0x22, v->render_vram[0x2000]);
    CHECK_EQ(cached(0x200), v->palette[7]);

    CHECK(!vdp_catch_up(v));  // nothing more: the third set of writes waits for a latch
    CHECK_EQ(0x22, v->render_vram[0x2000]);
    vdp_latch(v, 42, 1002);
    CHECK(vdp_catch_up(v));
    CHECK(render_in_step(v));
    free(v);
}

// A ring of VDP_LATCHES: the latches after it fills are merged into its newest,
// counted, and the render side jumps to the last of them.
TEST(a_full_ring_merges_into_its_newest) {
    vdp_t *v = new_card();
    for (unsigned n = 0; n < VDP_LATCHES + 3; n++) {
        set_reg(v, 0x07, (uint8_t)n);
        uint8_t byte = (uint8_t)(0x40 + n);
        store(v, (uint16_t)(0x3000 + n), &byte, 1);
        vdp_latch(v, (uint16_t)(100 + n), n);
    }
    CHECK_EQ(3, vdp_debug_stats(v).latches_merged);
    for (unsigned n = 0; n < VDP_LATCHES; n++) {
        CHECK(vdp_catch_up(v));
        unsigned latch = n < VDP_LATCHES - 1 ? n : VDP_LATCHES + 2;
        CHECK_EQ(latch, v->render_tag);
        CHECK_EQ(101 + latch, v->render_screen_line);
        CHECK_EQ(latch, v->render_reg[0x07]);
        CHECK_EQ(0x40 + latch, v->render_vram[0x3000 + latch]);
    }
    CHECK(!vdp_catch_up(v));
    CHECK(render_in_step(v));
    free(v);
}

// The overflow a catch-up's evaluation finds, and a build's collisions, reach
// status only through vdp_publish — which the firmware takes with the bus held
// off — and each only once.
TEST(status_waits_for_the_publish) {
    vdp_t *v = new_card();
    set_reg(v, 0x01, 0x40);   // display on, legacy Graphics I: 192 lines at screen line 24
    set_reg(v, 0x24, 1);      // SPRLIMIT 1
    set_reg(v, 0x0a, 0x0c);   // IRQEN: overflow and collision
    set_reg(v, 0x05, 0x00);   // SPRATTR at $0000
    for (unsigned n = 0; n < 2; n++) {
        uint8_t slot[] = {50, 100, 0, 0x0f};
        store(v, (uint16_t)(4 * n), slot, 4);
    }
    uint8_t terminator = 0xd0;
    store(v, 8, &terminator, 1);

    vdp_latch(v, 74, 0);      // builds display line 51: legacy sprites start at Y + 1
    CHECK(vdp_catch_up(v));
    CHECK_EQ(1, v->sprite_count);
    CHECK_EQ(0, v->stat0);
    CHECK(!vdp_int_asserted(v));
    vdp_publish(v, NULL, NULL);
    CHECK_EQ(0x41, v->stat0);
    CHECK_EQ(1, v->overflow_sprite);
    CHECK(vdp_int_asserted(v));
    CHECK_EQ(0x41, vdp_read(v, 1));
    vdp_publish(v, NULL, NULL);     // published once
    CHECK_EQ(0, v->stat0);
    free(v);
}

// Random traffic with the render side catching up late — for long stretches
// not at all, so the ring fills and merges — against a card that takes every
// latch whole the instant it falls (vdp_line_start). Each line the late card
// catches up must match the prompt card's render side at the same latch:
// registers, VRAM, the palette cache, the line's number and the sprites
// evaluated for it. Data-port reads must agree too. Status reads are left out:
// when a status reaches STAT0 is exactly what differs (vdp_publish). So are
// bursts past the journal: a page copy is the one thing a late catch-up takes
// from the bus copy as it is now.
TEST(late_catch_ups_take_what_prompt_ones_do) {
    vdp_t *late = new_card();
    vdp_t *prompt = new_card();
    enum { WRITE, READ, POKE, RST, LATCH };
    typedef struct {
        uint8_t kind, port, value;
        uint16_t address;
        uint32_t tag;
    } op_t;
    static op_t log[8192];
    size_t logged = 0;
    static const uint8_t registers[] = {0x01, 0x07, 0x08, 0x09, 0x0c, 0x0d, 0x10, 0x15, 0x16, 0x20, 0x21, 0x22, 0x23, 0x24};
    unsigned latches = 0, caught = 0, behind = 0, sprites = 0, dropped = 0, reads = 0;
    uint16_t screen = 0;
    bool in_step = true;
    random_state = 99;

    for (unsigned step = 0; step < 600000 && in_step; step++) {
        // Stretches of 1,000 steps with the render side left behind.
        bool lagging = (step / 1000) % 3 == 2;
        uint32_t roll = random_next() % 1000;
        unsigned pair = random_next() & 1;
        op_t ops[2];
        unsigned count = 1;
        if (roll < 100) {
            uint8_t reg = registers[random_next() % sizeof registers];
            // PALBASE among the top 8 KB; MODE1 with the display on, in any legacy mode.
            uint8_t value = reg == 0x0c   ? (uint8_t)(0x38 + random_next() % 8)
                            : reg == 0x01 ? (uint8_t)(0x40 | (random_next() & 0x1b))
                                          : (uint8_t)random_next();
            ops[0] = (op_t){WRITE, (uint8_t)(2 * pair + 1), value, 0, 0};
            ops[1] = (op_t){WRITE, (uint8_t)(2 * pair + 1), (uint8_t)(0x80 | reg), 0, 0};
            count = 2;
        } else if (roll < 160) {
            uint16_t address = random_next() & 1 ? (uint16_t)(0xe000 + random_next() % 0x2000) : (uint16_t)random_next();
            ops[0] = (op_t){WRITE, (uint8_t)(2 * pair + 1), (uint8_t)address, 0, 0};
            ops[1] = (op_t){WRITE, (uint8_t)(2 * pair + 1), (uint8_t)(((address >> 8) & 0x3f) | (random_next() & 0x40)), 0, 0};
            count = 2;
        } else if (roll < 500) {
            ops[0] = (op_t){WRITE, (uint8_t)(2 * pair), (uint8_t)random_next(), 0, 0};
        } else if (roll < 600) {
            ops[0] = (op_t){READ, (uint8_t)(2 * pair), 0, 0, 0};
        } else if (roll < 650) {
            ops[0] = (op_t){POKE, 0, (uint8_t)random_next(), (uint16_t)(random_next() & 1 ? 0xf000 + random_next() % 0x1000 : random_next() % 0x400), 0};
        } else if (roll < 651) {
            // RST pokes the palette's 512 bytes: only while the journal has room for them.
            if (lagging || late->journal_tail - late->journal_head > 256) continue;
            ops[0] = (op_t){RST, 0, 0, 0, 0};
        } else if (roll < 700) {
            screen = (uint16_t)((screen + 1) % VDP_SCREEN_LINES);
            ops[0] = (op_t){LATCH, 0, 0, screen, ++latches};
        } else {
            if ((lagging && roll < 995) || !vdp_catch_up(late)) continue;
            caught++;
            if (late->latch_tail != late->latch_head) behind++;
            if (late->sprite_count) sprites++;
            if (late->render_overflow) dropped++;
            // The prompt card plays the log up to that latch.
            size_t played = 0;
            for (;;) {
                const op_t *o = &log[played++];
                if (o->kind == WRITE) vdp_write(prompt, o->port, o->value);
                else if (o->kind == READ) CHECK_EQ(o->value, vdp_read(prompt, o->port));
                else if (o->kind == POKE) vdp_debug_set_vram(prompt, o->address, o->value);
                else if (o->kind == RST) vdp_reset(prompt, false);
                else {
                    prompt->stat0 = 0;  // so OVF below is this latch's alone
                    vdp_line_start(prompt, o->address);
                    if (o->tag == late->render_tag) break;
                }
            }
            memmove(log, log + played, (logged - played) * sizeof log[0]);
            logged -= played;

            in_step = memcmp(late->render_reg, prompt->render_reg, VDP_REGISTERS) == 0 &&
                      memcmp(late->render_vram, prompt->render_vram, VDP_VRAM_SIZE) == 0 &&
                      memcmp(late->palette, prompt->palette, sizeof late->palette) == 0 &&
                      late->render_screen_line == prompt->render_screen_line &&
                      late->sprite_count == prompt->sprite_count &&
                      memcmp(late->sprite, prompt->sprite, late->sprite_count * sizeof late->sprite[0]) == 0;
            CHECK(in_step);
            // The prompt card reported its overflow at the latch; the late one holds the same.
            CHECK_EQ(prompt->stat0 & 0x40 ? 1 : 0, late->render_overflow ? 1 : 0);
            vdp_publish(late, NULL, NULL);
            if (!in_step) {
                printf("  out of step at catch-up %u, render tag %u: reg %d vram %d palette %d line %u/%u sprites %u/%u overflows %u\n",
                       caught, late->render_tag, memcmp(late->render_reg, prompt->render_reg, VDP_REGISTERS) != 0,
                       memcmp(late->render_vram, prompt->render_vram, VDP_VRAM_SIZE) != 0,
                       memcmp(late->palette, prompt->palette, sizeof late->palette) != 0, late->render_screen_line,
                       prompt->render_screen_line, late->sprite_count, prompt->sprite_count, late->journal_overflows);
            }
            continue;
        }

        for (unsigned i = 0; i < count; i++) {
            op_t *o = &ops[i];
            if (o->kind == WRITE) vdp_write(late, o->port, o->value);
            else if (o->kind == READ) o->value = vdp_read(late, o->port), reads++;
            else if (o->kind == POKE) vdp_debug_set_vram(late, o->address, o->value);
            else if (o->kind == RST) vdp_reset(late, false);
            else vdp_latch(late, o->address, o->tag);
            if (logged == sizeof log / sizeof log[0]) {
                CHECK(logged < sizeof log / sizeof log[0]);
                in_step = false;
                break;
            }
            log[logged++] = *o;
        }
    }
    printf("  %u latches, %u caught up: %u with more waiting, %u with sprites, %u dropping one; %u merged; %u reads\n",
           latches, caught, behind, sprites, dropped, late->latches_merged, reads);
    CHECK_EQ(0, late->journal_overflows);
    CHECK(caught > 15000);
    CHECK(behind > caught / 5);
    CHECK(late->latches_merged > 500);
    CHECK(sprites > caught / 20);
    free(late);
    free(prompt);
}

int main(void) {
    RUN(operations_after_the_latch_wait_for_the_next);
    RUN(journal_overflow_copies_pages);
    RUN(palbase_moves_within_a_line);
    RUN(random_traffic_keeps_the_render_side_in_step);
    RUN(a_late_catch_up_takes_the_card_as_it_stood);
    RUN(a_full_ring_merges_into_its_newest);
    RUN(status_waits_for_the_publish);
    RUN(late_catch_ups_take_what_prompt_ones_do);
    return TEST_RESULT();
}
