// §2, §4, §6: the word the read program is staged with, and reads served from
// it (Phase 11).
//
// On the pins a read is answered before the firmware hears of it, from a word
// staged beforehand. vdp_staged is that word; vdp_read_served takes a read as
// what it returned. Served what vdp_read would return, it must be vdp_read
// exactly, which the host's Jest run and fuzzer then cover. Served a byte the
// card has since moved past, it must acknowledge only what it showed.

#include <string.h>

#include "card.h"
#include "test.h"

enum { IRQEN = 0x0a, STATSEL_B = 0x0e, STATSEL_A = 0x0f, VINC = 0x09 };

static uint8_t byte_of(uint32_t word, unsigned port) {
    return (uint8_t)(word >> (8 * port));
}

// A small seeded generator, the same everywhere.
static uint32_t lcg(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state >> 8;
}

// Each port's byte is what a read of that port returns now.
TEST(staged_is_what_each_port_reads) {
    vdp_t *v = new_card();
    uint8_t bytes[] = {0x5a, 0xa5, 0x3c};
    store(v, 0x4321, bytes, 3);
    point_at(v, 0, 0x4321, false);
    point_at(v, 1, 0x4322, false);
    set_reg(v, STATSEL_A, 4);
    set_reg(v, STATSEL_B, 6);
    uint32_t word = vdp_staged(v);
    CHECK_EQ(0x5a, byte_of(word, 0));
    CHECK_EQ(0xac, byte_of(word, 1));
    CHECK_EQ(0xa5, byte_of(word, 2));
    CHECK_EQ(0xbf, byte_of(word, 3));
    for (unsigned port = 0; port < 4; port++) {
        uint32_t now = vdp_staged(v);
        CHECK_EQ(byte_of(now, port), vdp_read(v, port));
    }
    free(v);
}

static uint8_t vram_a[VDP_VRAM_SIZE], vram_b[VDP_VRAM_SIZE];

static bool same_card(const vdp_t *a, const vdp_t *b) {
    vdp_snapshot_t sa, sb;
    memset(&sa, 0, sizeof sa);
    memset(&sb, 0, sizeof sb);
    vdp_debug_save(a, &sa, vram_a);
    vdp_debug_save(b, &sb, vram_b);
    return memcmp(&sa, &sb, sizeof sa) == 0 && memcmp(vram_a, vram_b, VDP_VRAM_SIZE) == 0;
}

// Served the staged byte, a read is vdp_read: the same value, the same card,
// through line starts that raise F and the interrupts and reads that
// acknowledge them on either port.
TEST(served_as_staged_is_a_read) {
    vdp_t *a = new_card(), *b = new_card();
    uint32_t seed = 11;
    uint16_t line = 0;
    int differing = 0;
    for (int i = 0; i < 200000; i++) {
        uint32_t roll = lcg(&seed) % 100;
        unsigned port = lcg(&seed) & 3;
        if (roll < 10) {
            line = (uint16_t)((line + 1 + lcg(&seed) % 40) % VDP_SCREEN_LINES);
            vdp_line_start(a, line);
            vdp_line_start(b, line);
        } else if (roll < 20) {
            static const uint8_t registers[] = {IRQEN, STATSEL_A, STATSEL_B, VINC, 0x0b, 0x08, 0x01};
            uint8_t r = registers[lcg(&seed) % sizeof registers];
            uint8_t value = (uint8_t)lcg(&seed);
            if (r == STATSEL_A || r == STATSEL_B) value &= 0x0f;
            vdp_write(a, port | 1, value);
            vdp_write(b, port | 1, value);
            vdp_write(a, port | 1, (uint8_t)(0x80 | r));
            vdp_write(b, port | 1, (uint8_t)(0x80 | r));
        } else if (roll < 40) {
            uint8_t value = (uint8_t)lcg(&seed);
            vdp_write(a, port, value);
            vdp_write(b, port, value);
        } else {
            uint8_t served = byte_of(vdp_staged(b), port);
            uint8_t want = vdp_read(a, port);
            uint8_t got = vdp_read_served(b, port, served);
            if (want != got || want != served) differing++;
        }
        if (i % 1000 == 0 && !same_card(a, b)) differing++;
    }
    CHECK_EQ(0, differing);
    CHECK(same_card(a, b));
    free(a);
    free(b);
}

// A flag that set after the word was staged is not cleared by the read that
// could not show it, and neither is its interrupt.
TEST(a_stale_stat0_keeps_what_it_did_not_show) {
    vdp_t *v = new_card();
    set_reg(v, IRQEN, 0x01);
    uint8_t served = byte_of(vdp_staged(v), 1);   // STAT0, F clear
    CHECK_EQ(0x00, served);
    for (uint16_t line = 0; line <= 240; line++) vdp_line_start(v, line);  // vertical blank
    CHECK(vdp_int_asserted(v));
    CHECK_EQ(0x80, vdp_read_served(v, 1, served));  // what a read now would return
    CHECK_EQ(0x80, vdp_debug_status(v, 0));
    CHECK(vdp_int_asserted(v));
    // The next read shows it, and clears it.
    served = byte_of(vdp_staged(v), 1);
    CHECK_EQ(0x80, vdp_read_served(v, 1, served));
    CHECK_EQ(0x00, vdp_debug_status(v, 0));
    CHECK(!vdp_int_asserted(v));
    free(v);
}

// One flag shown, two more set since: the one shown is cleared with its latch,
// the others keep theirs and what details them.
TEST(a_stale_stat0_clears_what_it_showed) {
    vdp_t *v = new_card();
    vdp_snapshot_t s;
    vdp_debug_save(v, &s, vram_a);
    s.registers[IRQEN] = 0x0d;                 // vblank, overflow, collision
    s.registers[0x01] |= 0x20;                 // MODE1 b5 is IRQEN b0
    s.stat0 = 0x80 | 0x40 | 0x20 | 0x05;       // F, OVF, COL, sprite 5
    s.overflow_sprite = 0x25;
    s.collision_map[0] = 0x21;
    s.irq_latch = 0x0d;
    vdp_debug_restore(v, &s, vram_a);

    CHECK_EQ(0xe5, vdp_read_served(v, 1, 0x80));  // served F alone
    CHECK_EQ(0x65, vdp_debug_status(v, 0));
    CHECK_EQ(0x25, vdp_debug_status(v, 7));
    CHECK_EQ(0x21, vdp_debug_status(v, 8));
    CHECK_EQ(0x0c, vdp_debug_status(v, 1));        // vblank's latch cleared

    CHECK_EQ(0x65, vdp_read_served(v, 1, 0x20));  // then COL alone
    CHECK_EQ(0x45, vdp_debug_status(v, 0));
    CHECK_EQ(0x00, vdp_debug_status(v, 8));
    CHECK_EQ(0x25, vdp_debug_status(v, 7));
    CHECK_EQ(0x04, vdp_debug_status(v, 1));

    CHECK_EQ(0x45, vdp_read_served(v, 1, 0x40));  // then OVF, with its index
    CHECK_EQ(0x00, vdp_debug_status(v, 0));
    CHECK_EQ(0x00, vdp_debug_status(v, 7));
    CHECK_EQ(0x00, vdp_debug_status(v, 1));
    CHECK(!vdp_int_asserted(v));
    free(v);
}

// STAT1 on port B: a latch that set after staging survives the read, and /INT
// with it; one IRQEN has since disabled goes, as a live read would take it.
TEST(a_stale_stat1_keeps_what_it_did_not_show) {
    vdp_t *v = new_card();
    vdp_snapshot_t s;
    vdp_debug_save(v, &s, vram_a);
    s.registers[IRQEN] = 0x03;
    s.registers[0x01] |= 0x20;
    s.registers[STATSEL_B] = 1;
    s.irq_latch = 0x02 | 0x08;                 // scanline, and a collision latched before IRQEN lost it
    vdp_debug_restore(v, &s, vram_a);
    CHECK_EQ(0x02, byte_of(vdp_staged(v), 3));
    v->irq_latch |= 0x01;                      // vertical blank, after the word was staged
    CHECK_EQ(0x03, vdp_read_served(v, 3, 0x02));
    CHECK_EQ(0x01, v->irq_latch);
    CHECK(vdp_int_asserted(v));
    free(v);
}

// A stale status read still resets its pair's flip-flop, and no other.
TEST(a_stale_read_resets_the_flip_flop) {
    vdp_t *v = new_card();
    vdp_snapshot_t s;
    vdp_debug_save(v, &s, vram_a);
    s.stat0 = 0x80;
    vdp_debug_restore(v, &s, vram_a);
    vdp_write(v, 1, 0x12);
    vdp_write(v, 3, 0x34);
    CHECK_EQ(0x80, vdp_read_served(v, 1, 0x00));
    CHECK(!vdp_debug_port(v, 0).second);
    CHECK(vdp_debug_port(v, 1).second);
    free(v);
}

int main(void) {
    RUN(staged_is_what_each_port_reads);
    RUN(served_as_staged_is_a_read);
    RUN(a_stale_stat0_keeps_what_it_did_not_show);
    RUN(a_stale_stat0_clears_what_it_showed);
    RUN(a_stale_stat1_keeps_what_it_did_not_show);
    RUN(a_stale_read_resets_the_flip_flop);
    return TEST_RESULT();
}
