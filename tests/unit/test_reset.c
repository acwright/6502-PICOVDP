// §15: reset, from power-on and from RST.

#include <string.h>

#include "card.h"
#include "test.h"

static void scribble(vdp_t *v) {
    for (unsigned reg = 0; reg < 128; reg++) set_reg(v, reg, (uint8_t)(0xff - reg));
    set_reg(v, 0x08, 0x00);
    set_reg(v, 0x09, 0x01);
    set_reg(v, 0x0c, 0x3c);  // the palette moved to $F000
    uint8_t bytes[] = {0xab, 0xcd};
    store(v, 0x0100, bytes, 2);
    store(v, 0xfc00, bytes, 2);  // entry 0 at $FC00, ordinary VRAM while PALBASE is $3C
    point_at(v, 1, 0x0101, false);
    vdp_write(v, 1, 0x42);       // A: half a command
}

static void check_reset_registers(const vdp_t *v) {
    static const uint8_t nonzero[][2] = {
        {0x09, 0x01}, {0x0c, 0x3f}, {0x15, 0x3c}, {0x1d, 0x0c}, {0x22, 0x20}, {0x23, 0x27}, {0x24, 0x10},
    };
    for (unsigned reg = 0; reg < 128; reg++) {
        uint8_t expected = 0;
        for (unsigned i = 0; i < sizeof nonzero / sizeof nonzero[0]; i++) {
            if (nonzero[i][0] == reg) expected = nonzero[i][1];
        }
        CHECK_EQ(expected, vdp_debug_register(v, reg));
    }
    for (unsigned pair = 0; pair < 2; pair++) {
        vdp_port_t p = vdp_debug_port(v, pair);
        CHECK_EQ(0, p.pointer);
        CHECK(p.read_mode);
        CHECK_EQ(0, p.prefetch);
        CHECK(!p.second);
        CHECK_EQ(0, p.payload);
    }
}

TEST(power_on) {
    vdp_t *v = new_card();
    check_reset_registers(v);
    scribble(v);
    vdp_reset(v, true);
    check_reset_registers(v);
    CHECK_EQ(0, vdp_debug_vram(v, 0x0100));
    CHECK_EQ(0, vdp_debug_vram(v, 0x0000));
    CHECK_EQ(0, vdp_debug_vram(v, 0xffff));
    CHECK_EQ(0xfff, vdp_debug_palette(v, 15));
    // Nothing pending: the render side is the bus side already.
    CHECK(memcmp(v->render_vram, v->vram, sizeof v->vram) == 0);
    CHECK(memcmp(v->render_reg, v->reg, sizeof v->reg) == 0);
    CHECK_EQ(0, v->journal_tail - v->journal_head);
    CHECK_EQ(0, v->latch_tail - v->latch_head);
    // STAT5 is the version the card was made with, not a register.
    set_reg(v, 0x0f, 0x05);
    CHECK_EQ(0x04, vdp_read(v, 1));
    free(v);
}

TEST(rst_keeps_vram_but_the_palette) {
    vdp_t *v = new_card();
    scribble(v);
    vdp_line_start(v, 100);
    vdp_reset(v, false);
    check_reset_registers(v);
    CHECK_EQ(0xab, vdp_debug_vram(v, 0x0100));
    CHECK_EQ(0xcd, vdp_debug_vram(v, 0x0101));
    CHECK_EQ(0x000, vdp_debug_palette(v, 0));  // at $FC00 again, over the scribble
    CHECK_EQ(0x2c4, vdp_debug_palette(v, 2));

    // The raster runs on: the line being built still sees the card before RST,
    // and the next latch brings the reset through.
    CHECK_EQ(0x3c, v->render_reg[0x0c]);
    CHECK_EQ(0xab, v->render_vram[0xfc00]);
    vdp_line_start(v, 101);
    CHECK(memcmp(v->render_vram, v->vram, sizeof v->vram) == 0);
    CHECK(memcmp(v->render_reg, v->reg, sizeof v->reg) == 0);
    CHECK_EQ(0x00, v->render_vram[0xfc00]);
    CHECK_EQ(0x0fffu * 0x10001u, v->palette[15]);
    CHECK_EQ(101, v->screen_line);
    free(v);
}

int main(void) {
    RUN(power_on);
    RUN(rst_keeps_vram_but_the_palette);
    return TEST_RESULT();
}
