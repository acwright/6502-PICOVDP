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
    CHECK_EQ(VDP_JOURNAL_ENTRIES, v->journal_count);
    CHECK(v->dirty_pages != 0);

    vdp_line_start(v, 1);
    CHECK(render_in_step(v));
    CHECK_EQ(0x9ab, (uint16_t)(((v->render_vram[0xfc42] & 0xf) << 8) | v->render_vram[0xfc43]));
    CHECK_EQ(cached(0x9ab), v->palette[0x21]);
    CHECK_EQ(1, vdp_debug_stats(v).journal_overflows);
    CHECK_EQ(0, v->journal_count);
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

int main(void) {
    RUN(operations_after_the_latch_wait_for_the_next);
    RUN(journal_overflow_copies_pages);
    RUN(palbase_moves_within_a_line);
    RUN(random_traffic_keeps_the_render_side_in_step);
    return TEST_RESULT();
}
