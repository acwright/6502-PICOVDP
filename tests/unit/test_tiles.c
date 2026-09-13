// §8's tile engine at 1bpp, §9's geometries and legacy submode, and the
// backdrop and border (§3, §11), at the core's own interface: the platform
// names a screen line, and vdp_build_line makes the row after it.
//
// Video.test.ts covers the picture through the adapter, which builds each row
// the instant its latch falls. What it cannot see is here: that the row built
// is the one after the latch's, that a bus write during the build does not
// reach it, and that every table address wraps at 64 KB.

#include <string.h>

#include "card.h"
#include "test.h"

enum {
    MODE1 = 0x01, L0NAME_LEGACY = 0x02, L0ATTR_LEGACY = 0x03, L0PAT_LEGACY = 0x04, COLOR = 0x07,
    VMODE = 0x0d, L0NAME = 0x10, L0ATTR = 0x11, L0PAT = 0x12, L0SCRX = 0x13, L0CTRL = 0x15, L0PAL = 0x16,
    L0SCRY = 0x14, L1NAME = 0x18, L1ATTR = 0x19, L1PAT = 0x1a, L1CTRL = 0x1d, L1PAL = 0x1e,
};

enum { DISP = 0x40, M1 = 0x10, ENABLE = 0x10, OPAQUE = 0x20 };

// The row screen line `latch` + 1 shows, built as `latch` begins (§3).
static void row_after(vdp_t *v, uint16_t latch, uint8_t *row) {
    vdp_line_start(v, latch);
    vdp_build_line(v, row);
}

static void poke(vdp_t *v, uint16_t address, uint8_t value) {
    vdp_debug_set_vram(v, address, value);
}

// Every index of [from, to) is `index`.
static bool all(const uint8_t *row, unsigned from, unsigned to, uint8_t index) {
    for (unsigned x = from; x < to; x++) {
        if (row[x] != index) return false;
    }
    return true;
}

// The BIOS's console: legacy Text, COLOR = $1F, patterns at $0800 (§7).
static vdp_t *console(void) {
    vdp_t *v = new_card();
    set_reg(v, MODE1, DISP | M1);
    set_reg(v, COLOR, 0x1f);
    set_reg(v, L0PAT_LEGACY, 0x01);
    return v;
}

TEST(the_bios_console) {
    vdp_t *v = console();
    uint8_t row[VDP_WIDTH];
    poke(v, 0x0800 + 'A' * 8, 0xff);        // row 0 of 'A': all eight bits set
    poke(v, 0x0000, 'A');                   // cell (0, 0)
    poke(v, 39, 'A');                       // cell (39, 0)

    // Screen line 24 is display line 0 (§3). Six pixels of the cell, not eight
    // (§8): the next cell's pattern 0 starts at x 46.
    row_after(v, 23, row);
    CHECK(all(row, 0, 40, 15));             // border: the backdrop, COLOR b3:0
    CHECK(all(row, 40, 46, 1));             // foreground nibble
    CHECK(all(row, 46, 274, 15));
    CHECK(all(row, 274, 280, 1));           // cell 39
    CHECK(all(row, 280, VDP_WIDTH, 15));

    // Display line 1 is pattern row 1, which is clear.
    row_after(v, 24, row);
    CHECK(all(row, 0, VDP_WIDTH, 15));

    // The top border (display line 261), the bottom border (192) and blanking.
    set_reg(v, COLOR, 0x14);
    row_after(v, 22, row);
    CHECK(all(row, 0, VDP_WIDTH, 4));
    row_after(v, 215, row);
    CHECK(all(row, 0, VDP_WIDTH, 4));
    row_after(v, 250, row);
    CHECK(all(row, 0, VDP_WIDTH, 4));
    free(v);
}

// §8, §9: a nibble of 0 is transparent — to the backdrop, here — and the
// legacy submode ignores L0CTRL b5, which resets set. VMODE's modes honour it.
TEST(a_nibble_of_zero_is_transparent_unless_index_0_is_opaque) {
    vdp_t *v = new_card();
    uint8_t row[VDP_WIDTH];
    set_reg(v, MODE1, DISP);                // legacy Graphics I; every cell names pattern 0
    set_reg(v, COLOR, 0x04);                // backdrop 4
    set_reg(v, L0ATTR_LEGACY, 0x40);        // $40 x $40 = $1000
    set_reg(v, L0PAT_LEGACY, 0x01);         // $0800
    poke(v, 0x1000, 0x30);                  // patterns 0-7: fg 3, bg 0
    poke(v, 0x0800, 0xf0);                  // pattern 0, row 0

    CHECK_EQ(0x3c, vdp_debug_register(v, L0CTRL));  // b5 set at reset
    row_after(v, 23, row);
    CHECK(all(row, 32, 36, 3));
    CHECK(all(row, 36, 40, 4));             // transparent: the backdrop

    // Compact through VMODE, per pattern group: now L0ATTR is x $400 and b5 counts.
    set_reg(v, VMODE, 0x02);
    set_reg(v, L0CTRL, ENABLE | OPAQUE | (1 << 2));
    set_reg(v, L0ATTR, 0x04);               // $1000
    set_reg(v, L0PAL, 0x02);
    row_after(v, 23, row);
    CHECK(all(row, 32, 36, 0x23));
    CHECK(all(row, 36, 40, 0x20));          // entry 0 of the group, drawn
    CHECK(all(row, 0, 32, 0x24));           // the backdrop takes L0PAL too (§11)

    set_reg(v, L0CTRL, ENABLE | (1 << 2));
    row_after(v, 23, row);
    CHECK(all(row, 36, 40, 0x24));
    free(v);
}

// §8's four attribute sources, each finding its own byte and no other, at 1bpp.
TEST(each_attribute_source_finds_its_byte) {
    vdp_t *v = new_card();
    uint8_t row[VDP_WIDTH];
    set_reg(v, MODE1, DISP);
    set_reg(v, VMODE, 0x02);                // Compact: 32 x 24 of 8 x 8 at (32, 24)
    set_reg(v, COLOR, 0x9e);
    set_reg(v, L0ATTR, 0x04);               // $1000
    set_reg(v, L0PAT, 0x08);                // $4000

    enum { PATTERN = 0x2b, COL = 5, CELL_ROW = 2, PIXEL_ROW = 3 };
    poke(v, (uint16_t)(CELL_ROW * 32 + COL), PATTERN);
    poke(v, (uint16_t)(0x4000 + PATTERN * 8 + PIXEL_ROW), 0xf0);
    poke(v, (uint16_t)(0x1000 + CELL_ROW * 32 + COL), 0x12);         // per cell
    poke(v, (uint16_t)(0x1000 + (PATTERN >> 3)), 0x34);              // per pattern group
    poke(v, (uint16_t)(0x1000 + PATTERN * 8 + PIXEL_ROW), 0x56);     // per pattern row

    static const struct { uint8_t source, fg, bg; } sources[] = {
        {0, 0x1, 0x2}, {1, 0x3, 0x4}, {2, 0x5, 0x6}, {3, 0x9, 0xe},
    };
    uint16_t latch = 24 + CELL_ROW * 8 + PIXEL_ROW - 1;
    unsigned x = 32 + COL * 8;
    for (unsigned i = 0; i < 4; i++) {
        set_reg(v, L0CTRL, (uint8_t)(ENABLE | OPAQUE | (sources[i].source << 2)));
        row_after(v, latch, row);
        CHECK(all(row, x, x + 4, sources[i].fg));
        CHECK(all(row, x + 4, x + 8, sources[i].bg));
    }

    // The legacy submode's group table: L0ATTR x $40 (§9).
    set_reg(v, VMODE, 0x00);
    set_reg(v, L0ATTR_LEGACY, 0x04);        // $0100
    set_reg(v, L0NAME_LEGACY, 0x00);
    set_reg(v, L0PAT_LEGACY, 0x08);
    poke(v, (uint16_t)(0x0100 + (PATTERN >> 3)), 0x78);
    row_after(v, latch, row);
    CHECK(all(row, x, x + 4, 0x7));
    CHECK(all(row, x + 4, x + 8, 0x8));
    free(v);
}

// §5, §7: a table whose base plus offset passes $FFFF continues at $0000 —
// the name table, the pattern table and the attribute table alike.
TEST(every_table_wraps_at_64kb) {
    vdp_t *v = new_card();
    uint8_t row[VDP_WIDTH];
    set_reg(v, MODE1, DISP);
    set_reg(v, VMODE, 0x04);                // Full: 40 x 30, no border
    set_reg(v, L0CTRL, ENABLE | OPAQUE | (2 << 2));  // per pattern row
    set_reg(v, L0NAME, 0x3f);               // $FC00: cell row 29 is at $FC00 + 1160 = $0088
    set_reg(v, L0PAT, 0xff);                // $F800: pattern $FF's row 7 is $FFFF
    set_reg(v, L0ATTR, 0x3f);               // $FC00: pattern $FF's row 7 attribute is $03FF
    set_reg(v, 0x0c, 0x30);                 // the palette out of the way, at $C000

    poke(v, 0x0088, 0xff);
    poke(v, 0xffff, 0xaa);
    poke(v, 0x03ff, 0x5c);
    row_after(v, 238, row);                 // screen and display line 239
    for (unsigned x = 0; x < 8; x++) CHECK_EQ(x & 1 ? 0xc : 0x5, row[x]);
    free(v);
}

// §3: vdp_line_start(s) latches the row of screen line s + 1, numbered as a
// display line by the geometry of the moment. Line 0 is built as 261 begins.
TEST(the_row_built_is_the_one_after_the_latch) {
    static const struct { uint8_t vmode; uint16_t origin_x, origin_y, lines; } modes[] = {
        {0x02, 32, 24, 192}, {0x03, 32, 0, 240},
    };
    for (unsigned m = 0; m < 2; m++) {
        vdp_t *v = new_card();
        uint8_t row[VDP_WIDTH];
        set_reg(v, MODE1, DISP);
        set_reg(v, VMODE, modes[m].vmode);
        set_reg(v, COLOR, 0x00);            // backdrop 0
        set_reg(v, L0CTRL, ENABLE | (0 << 2));  // per cell
        set_reg(v, L0ATTR, 0x04);           // $1000
        set_reg(v, L0PAT, 0x08);            // $4000
        // Column 0 of cell row n: a solid pattern, in a foreground of its own.
        for (unsigned r = 0; r < 8; r++) poke(v, (uint16_t)(0x4000 + 8 + r), 0xff);
        for (unsigned n = 0; n < 30; n++) {
            poke(v, (uint16_t)(n * 32), 1);
            poke(v, (uint16_t)(0x1000 + n * 32), (uint8_t)(((n % 15) + 1) << 4));
        }

        for (unsigned latch = 0; latch < VDP_SCREEN_LINES; latch++) {
            row_after(v, (uint16_t)latch, row);
            unsigned screen = (latch + 1) % VDP_SCREEN_LINES;
            unsigned display = (screen + VDP_SCREEN_LINES - modes[m].origin_y) % VDP_SCREEN_LINES;
            bool picture = screen < VDP_HEIGHT && display < modes[m].lines;
            CHECK_EQ(0, row[modes[m].origin_x - 1]);
            CHECK_EQ(picture ? (display / 8) % 15 + 1 : 0, row[modes[m].origin_x]);
        }
        free(v);
    }
}

// The one Jest cannot see (PLAN.md section 3): the bus, between the latch and
// the build, reaches nothing the build reads — not VRAM, not the layer's
// registers, not the mode. The next latch takes all of it.
TEST(a_write_during_the_build_waits_for_the_next_line) {
    vdp_t *v = console();
    uint8_t before[VDP_WIDTH], during[VDP_WIDTH], after[VDP_WIDTH];
    for (unsigned c = 0; c < 40; c++) poke(v, (uint16_t)c, (uint8_t)c);
    for (unsigned p = 0; p < 40; p++) poke(v, (uint16_t)(0x0800 + p * 8), (uint8_t)(p * 37 + 11));

    row_after(v, 23, before);

    vdp_line_start(v, 23);
    uint8_t zeros[16] = {0};
    store(v, 0x0800, zeros, sizeof zeros);  // patterns 0 and 1
    store(v, 0x0000, zeros, 4);             // cells 0-3
    set_reg(v, COLOR, 0x4a);
    set_reg(v, L0SCRX, 5);
    set_reg(v, L0CTRL, 0x00);               // disabled
    set_reg(v, VMODE, 0x04);                // Full
    set_reg(v, MODE1, 0x00);                // display off
    vdp_build_line(v, during);
    CHECK(memcmp(before, during, VDP_WIDTH) == 0);

    row_after(v, 24, after);                // display off: the new backdrop, everywhere
    CHECK(all(after, 0, VDP_WIDTH, 0xa));
    free(v);
}

// §3: display off draws the backdrop over the picture; a disabled layer draws
// nothing, and the other layer still draws.
TEST(display_off_and_a_disabled_layer) {
    vdp_t *v = console();
    uint8_t row[VDP_WIDTH];
    poke(v, 0x0800, 0xff);                  // pattern 0, row 0: every cell is solid

    row_after(v, 23, row);
    CHECK(all(row, 40, 280, 1));

    set_reg(v, MODE1, M1);                  // display off
    row_after(v, 23, row);
    CHECK(all(row, 0, VDP_WIDTH, 15));

    set_reg(v, MODE1, DISP | M1);
    set_reg(v, L0CTRL, 0x2f);               // b4 clear
    row_after(v, 23, row);
    CHECK(all(row, 0, VDP_WIDTH, 15));

    // Layer 1, over the disabled layer 0: its own tables, COLOR's pair in
    // L1PAL's group.
    set_reg(v, L1CTRL, ENABLE | OPAQUE | (3 << 2));
    set_reg(v, L1PAT, 0x08);                // $4000: pattern 0 row 0 clear
    set_reg(v, L1PAL, 0x01);
    row_after(v, 23, row);
    CHECK(all(row, 40, 280, 0x1f));         // COLOR's background nibble, in group 1
    CHECK(all(row, 0, 40, 15));             // the backdrop is L0PAL's (§11)
    free(v);
}

// §12 at 1bpp: layer 1 over layer 0, each pixel a transparent layer 1 leaves
// showing layer 0, and one layer 0 leaves showing the backdrop.
TEST(layer_1_draws_over_layer_0) {
    vdp_t *v = new_card();
    uint8_t row[VDP_WIDTH];
    set_reg(v, MODE1, DISP);
    set_reg(v, VMODE, 0x02);
    set_reg(v, COLOR, 0x07);
    set_reg(v, L0CTRL, ENABLE | (0 << 2));  // per cell, index 0 transparent
    set_reg(v, L0ATTR, 0x04);               // $1000
    set_reg(v, L0PAT, 0x08);                // $4000
    set_reg(v, L1CTRL, ENABLE | (0 << 2));
    set_reg(v, L1NAME, 0x08);               // $2000
    set_reg(v, L1ATTR, 0x0c);               // $3000
    set_reg(v, L1PAT, 0x10);                // $8000

    poke(v, 0x0000, 1);
    poke(v, 0x1000, 0x30);                  // fg 3, bg transparent
    poke(v, 0x4000 + 8, 0x3c);              // pattern 1: ..XXXX..
    poke(v, 0x2000, 2);
    poke(v, 0x3000, 0x50);
    poke(v, 0x8000 + 16, 0xc0);             // pattern 2: XX......

    row_after(v, 23, row);
    static const uint8_t expected[8] = {5, 5, 3, 3, 3, 3, 7, 7};
    CHECK(memcmp(row + 32, expected, 8) == 0);

    set_reg(v, L1CTRL, ENABLE | OPAQUE);    // layer 1's index 0 drawn: it occludes
    set_reg(v, L1PAL, 0x06);
    row_after(v, 23, row);
    static const uint8_t occluded[8] = {0x65, 0x65, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60};
    CHECK(memcmp(row + 32, occluded, 8) == 0);

    // §9 pins layer 0 alone: in the legacy submode layer 1 keeps its own depth,
    // attribute source, x $400 granule and opacity.
    set_reg(v, VMODE, 0x00);
    row_after(v, 23, row);
    CHECK(memcmp(row + 32, occluded, 8) == 0);
    free(v);
}

// §13 at 1bpp: a scroll that is not a multiple of the cell splits the first
// cell partway into its pattern byte; X is nine bits; Y moves the pattern row.
TEST(a_scroll_splits_a_cell_partway_into_its_pattern) {
    vdp_t *v = new_card();
    uint8_t row[VDP_WIDTH];
    set_reg(v, MODE1, DISP);
    set_reg(v, VMODE, 0x04);                // Full: 320 wide, origin (0, 0)
    set_reg(v, COLOR, 0x10);                // COLOR's pair: fg 1, bg 0
    set_reg(v, L0CTRL, ENABLE | OPAQUE | (3 << 2));
    set_reg(v, L0PAT, 0x08);                // $4000
    poke(v, 0x0000 + 1, 1);                 // cell (1, 0): pattern 1
    poke(v, 0x4000 + 8, 0xc9);              // pattern 1, row 0: XX..X..X
    poke(v, 0x4000 + 8 + 5, 0xff);          // row 5: solid

    set_reg(v, L0SCRX, 11);                 // x 0 is pixel 3 of cell 1
    row_after(v, 261, row);
    static const uint8_t split[6] = {0, 1, 0, 0, 1, 0};
    CHECK(memcmp(row, split, 5) == 0);
    CHECK_EQ(0, row[5]);                    // cell 2: pattern 0

    set_reg(v, L0SCRX, 0x09);               // with b6: 256 + 9 = 265, cell 33 pixel 1
    set_reg(v, L0CTRL, ENABLE | OPAQUE | (3 << 2) | 0x40);
    poke(v, 33, 2);                         // cell 33: pattern 2
    poke(v, 0x4000 + 16, 0x5a);             // .X.XX.X.
    poke(v, 0x4000 + 16 + 5, 0xff);
    row_after(v, 261, row);
    static const uint8_t ninth[7] = {1, 0, 1, 1, 0, 1, 0};  // not cell 1's 1, 0, 0, 1, 0, 0, 1
    CHECK(memcmp(row, ninth, 7) == 0);

    set_reg(v, L0SCRY, 5);                  // display line 0 is pattern row 5
    row_after(v, 261, row);
    CHECK(all(row, 0, 7, 1));
    free(v);
}

// A restored snapshot builds the row after the screen line it was saved on.
TEST(a_restored_card_builds_the_row_it_was_saved_at) {
    vdp_t *v = console();
    vdp_t *w = new_card();
    uint8_t row[VDP_WIDTH], restored[VDP_WIDTH];
    static uint8_t vram[VDP_VRAM_SIZE];
    vdp_snapshot_t s;
    for (unsigned c = 0; c < 40; c++) poke(v, (uint16_t)c, (uint8_t)(c + 1));
    for (unsigned p = 1; p <= 40; p++) poke(v, (uint16_t)(0x0800 + p * 8), (uint8_t)(p * 29));

    row_after(v, 23, row);
    vdp_debug_save(v, &s, vram);
    vdp_debug_restore(w, &s, vram);
    vdp_build_line(w, restored);
    CHECK(memcmp(row, restored, VDP_WIDTH) == 0);
    CHECK(!all(row, 40, 280, row[40]));
    free(v);
    free(w);
}

int main(void) {
    RUN(the_bios_console);
    RUN(a_nibble_of_zero_is_transparent_unless_index_0_is_opaque);
    RUN(each_attribute_source_finds_its_byte);
    RUN(every_table_wraps_at_64kb);
    RUN(the_row_built_is_the_one_after_the_latch);
    RUN(a_write_during_the_build_waits_for_the_next_line);
    RUN(display_off_and_a_disabled_layer);
    RUN(layer_1_draws_over_layer_0);
    RUN(a_scroll_splits_a_cell_partway_into_its_pattern);
    RUN(a_restored_card_builds_the_row_it_was_saved_at);
    return TEST_RESULT();
}
