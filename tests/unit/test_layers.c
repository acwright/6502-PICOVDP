// §8 at 2, 4 and 8bpp — the attribute byte, the palette mapping, §18's 4bpp
// unpacking table — and §12's seven levels, at the core's own interface.
//
// Video.test.ts covers the depths and priority through the adapter. What is
// here is what it reaches only in part: every value of every group through the
// 4bpp table, a flip in Text's six-pixel cell and in a partial cell, the
// attribute byte found by the row before its flip, the contest between the
// layers on a line with no sprites (where the build keeps levels only because
// layer 0 can carry b6) and with them, and pattern addresses that wrap at
// 64 KB.

#include <string.h>

#include "card.h"
#include "test.h"

enum {
    MODE1 = 0x01, COLOR = 0x07, PALBASE = 0x0c, VMODE = 0x0d,
    L0NAME = 0x10, L0ATTR = 0x11, L0PAT = 0x12, L0SCRX = 0x13, L0SCRY = 0x14, L0CTRL = 0x15, L0PAL = 0x16,
    L1NAME = 0x18, L1ATTR = 0x19, L1PAT = 0x1a, L1CTRL = 0x1d, L1PAL = 0x1e,
    SPRATTR = 0x20, SPRPAT = 0x21, SPRCTRL = 0x23,
};

enum { DISP = 0x40, ENABLE = 0x10, OPAQUE = 0x20, SCRX_BIT8 = 0x40 };
enum { BPP1 = 0, BPP2 = 1, BPP4 = 2, BPP8 = 3 };
enum { PER_CELL = 0 << 2, PER_GROUP = 1 << 2, PER_ROW = 2 << 2, NONE = 3 << 2 };
enum { FLIP_X = 0x10, FLIP_Y = 0x20, PRIORITY = 0x40, BIT8 = 0x80 };

// Where full() puts the tables.
enum { L0_NAMES = 0x0000, L0_ATTRS = 0x1000, L0_PATTERNS = 0x2000 };
enum { L1_NAMES = 0x8000, L1_ATTRS = 0x9000, L1_PATTERNS = 0xa000 };
enum { BACKDROP = 14 };

static void poke(vdp_t *v, uint16_t address, uint8_t value) {
    vdp_debug_set_vram(v, address, value);
}

static void pokes(vdp_t *v, uint16_t address, const uint8_t *bytes, size_t count) {
    for (size_t i = 0; i < count; i++) poke(v, (uint16_t)(address + i), bytes[i]);
}

// Full, VMODE $4: 320 x 240 from (0, 0), so picture column x is frame column x
// and display line n is screen line n, built as n - 1 begins. Layer 0's tables
// at $0000, $1000 and $2000, layer 1's at $8000, $9000 and $A000, disabled;
// the palette out of their way at $F000; sprites off; backdrop 14.
static vdp_t *full(void) {
    vdp_t *v = new_card();
    set_reg(v, MODE1, DISP);
    set_reg(v, VMODE, 0x04);
    set_reg(v, PALBASE, 0x3c);
    set_reg(v, SPRCTRL, 0x26);
    set_reg(v, COLOR, BACKDROP);
    set_reg(v, L0ATTR, 0x04);
    set_reg(v, L0PAT, 0x04);
    set_reg(v, L1NAME, 0x20);
    set_reg(v, L1ATTR, 0x24);
    set_reg(v, L1PAT, 0x14);
    return v;
}

// Display line `line` of a Full picture.
static void line_of(vdp_t *v, uint16_t line, uint8_t *row) {
    vdp_line_start(v, (uint16_t)((line + VDP_SCREEN_LINES - 1) % VDP_SCREEN_LINES));
    vdp_build_line(v, row);
}

static bool all(const uint8_t *row, unsigned from, unsigned to, uint8_t index) {
    for (unsigned x = from; x < to; x++) {
        if (row[x] != index) return false;
    }
    return true;
}

// §8: pixels leftmost first, the high bits or nibble of each byte first, and
// each depth's own palette mapping: at 2bpp (LxPAL & 3) x 64 + sub x 4 + value,
// at 4bpp sub x 16 + value, at 8bpp the value.
TEST(each_depth_decodes_and_maps_its_palette) {
    vdp_t *v = full();
    uint8_t row[VDP_WIDTH];
    set_reg(v, L0PAL, 0x07);                // quarter 3 at 2bpp; nothing at 4 and 8bpp
    poke(v, L0_NAMES, 5);
    poke(v, L0_ATTRS, 0x0b);                // sub-palette 11

    set_reg(v, L0CTRL, ENABLE | OPAQUE | PER_CELL | BPP2);
    static const uint8_t two[2] = {0x1b, 0xe4};  // 0 1 2 3 3 2 1 0
    pokes(v, (uint16_t)(L0_PATTERNS + 5 * 16), two, 2);
    line_of(v, 0, row);
    static const uint8_t two_expected[8] = {236, 237, 238, 239, 239, 238, 237, 236};
    CHECK(memcmp(row, two_expected, 8) == 0);

    set_reg(v, L0CTRL, ENABLE | OPAQUE | PER_CELL | BPP4);
    static const uint8_t four[4] = {0x01, 0x23, 0x45, 0x67};
    pokes(v, (uint16_t)(L0_PATTERNS + 5 * 32), four, 4);
    line_of(v, 0, row);
    for (unsigned x = 0; x < 8; x++) CHECK_EQ(0xb0 + x, row[x]);

    set_reg(v, L0CTRL, ENABLE | OPAQUE | PER_CELL | BPP8);
    static const uint8_t eight[8] = {0x00, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xff};
    pokes(v, (uint16_t)(L0_PATTERNS + 5 * 64 + 8), eight, 8);  // row 1
    line_of(v, 1, row);
    CHECK(memcmp(row, eight, 8) == 0);      // sub-palette and LxPAL both ignored
    free(v);
}

// §8's transparency at 2, 4 and 8bpp: a value of 0 within its group is not
// drawn unless index 0 is opaque — whatever index the group puts it at.
TEST(value_0_is_transparent_unless_index_0_is_opaque) {
    static const struct { uint8_t depth, bytes; uint8_t pattern[8]; uint8_t drawn[8]; } depths[] = {
        {BPP2, 2, {0x0c, 0x30}, {0, 0, 0x4b, 0, 0, 0x4b, 0, 0}},                    // group 18: 72 + v
        {BPP4, 4, {0x05, 0x00, 0x50, 0x00}, {0, 0x25, 0, 0, 0x25, 0, 0, 0}},        // (18 x 16 + v) & $FF: 32 + v
        {BPP8, 8, {0, 0, 7, 0, 0, 0, 0, 9}, {0, 0, 7, 0, 0, 0, 0, 9}},
    };
    for (unsigned d = 0; d < 3; d++) {
        vdp_t *v = full();
        uint8_t row[VDP_WIDTH];
        set_reg(v, L0PAL, 0x01);
        poke(v, L0_ATTRS, 0x02);
        pokes(v, L0_PATTERNS, depths[d].pattern, depths[d].bytes);
        set_reg(v, L0CTRL, (uint8_t)(ENABLE | PER_CELL | depths[d].depth));
        line_of(v, 0, row);
        uint8_t group_zero = depths[d].depth == BPP2 ? 0x48 : depths[d].depth == BPP4 ? 0x20 : 0x00;
        for (unsigned x = 0; x < 8; x++) CHECK_EQ(depths[d].drawn[x] ? depths[d].drawn[x] : 0x10 | BACKDROP, row[x]);

        set_reg(v, L0CTRL, (uint8_t)(ENABLE | OPAQUE | PER_CELL | depths[d].depth));
        line_of(v, 0, row);
        for (unsigned x = 0; x < 8; x++) CHECK_EQ(depths[d].drawn[x] ? depths[d].drawn[x] : group_zero, row[x]);
        free(v);
    }
}

// §18's 4bpp table against §8's arithmetic: every pattern byte in every
// sub-palette, opaque and not. Tile t's row 0 is bytes 4t to 4t + 3, cells 0-39
// name tiles 0-39 and cells 40-63 of the next cell row name tiles 40-63.
TEST(the_4bpp_table_is_the_mapping) {
    vdp_t *v = full();
    uint8_t row[VDP_WIDTH];
    for (unsigned t = 0; t < 64; t++) {
        poke(v, (uint16_t)(L0_NAMES + t), (uint8_t)t);
        for (unsigned b = 0; b < 4; b++) poke(v, (uint16_t)(L0_PATTERNS + t * 32 + b), (uint8_t)(4 * t + b));
    }
    unsigned checked = 0;
    for (unsigned opaque = 0; opaque < 2; opaque++) {
        set_reg(v, L0CTRL, (uint8_t)(ENABLE | (opaque ? OPAQUE : 0) | PER_CELL | BPP4));
        for (unsigned group = 0; group < 16; group++) {
            for (unsigned cell = 0; cell < 80; cell++) poke(v, (uint16_t)(L0_ATTRS + cell), (uint8_t)group);
            for (unsigned cell_row = 0; cell_row < 2; cell_row++) {
                line_of(v, (uint16_t)(8 * cell_row), row);
                unsigned cells = cell_row ? 24 : 40;
                for (unsigned c = 0; c < cells; c++) {
                    uint8_t byte = (uint8_t)(4 * (40 * cell_row + c));
                    for (unsigned b = 0; b < 4; b++, byte++) {
                        for (unsigned pixel = 0; pixel < 2; pixel++) {
                            unsigned value = pixel ? byte & 15 : byte >> 4;
                            unsigned expected = value || opaque ? group * 16 + value : BACKDROP;
                            CHECK_EQ(expected, row[8 * c + 2 * b + pixel]);
                            checked++;
                        }
                    }
                }
            }
        }
    }
    CHECK_EQ(2 * 16 * 256 * 2, checked);
    free(v);
}

// §8: b4 mirrors a cell across, b5 top to bottom, both at once turn it round.
// In Text only the leftmost six pixels are drawn, and those six are mirrored;
// a partial cell from a scroll is mirrored as the whole cell is.
TEST(flips_mirror_the_cell) {
    vdp_t *v = full();
    uint8_t row[VDP_WIDTH];
    set_reg(v, L0CTRL, ENABLE | OPAQUE | PER_CELL | BPP8);
    static const uint8_t top[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    static const uint8_t bottom[8] = {11, 12, 13, 14, 15, 16, 17, 18};
    pokes(v, L0_PATTERNS, top, 8);
    pokes(v, (uint16_t)(L0_PATTERNS + 7 * 8), bottom, 8);

    static const struct { uint8_t attribute; uint16_t line; uint8_t first, step; } cases[] = {
        {0, 0, 1, 1}, {FLIP_X, 0, 8, 0xff}, {FLIP_Y, 0, 11, 1}, {FLIP_X | FLIP_Y, 0, 18, 0xff}, {FLIP_Y, 7, 1, 1},
    };
    for (unsigned i = 0; i < 5; i++) {
        poke(v, L0_ATTRS, cases[i].attribute);
        line_of(v, cases[i].line, row);
        for (unsigned x = 0; x < 8; x++) CHECK_EQ((uint8_t)(cases[i].first + cases[i].step * x), row[x]);
    }

    // A scroll of 3: column 0 is pixel 3 of cell 0, which flipped is pattern pixel 4.
    set_reg(v, L0SCRX, 3);
    poke(v, L0_ATTRS, FLIP_X);
    line_of(v, 0, row);
    static const uint8_t partial[5] = {5, 4, 3, 2, 1};
    CHECK(memcmp(row, partial, 5) == 0);
    set_reg(v, L0SCRX, 0);

    // Text, VMODE $1: 6 x 8 cells from (40, 24). Six pixels, mirrored as six.
    set_reg(v, VMODE, 0x01);
    poke(v, L0_ATTRS, 0);
    vdp_line_start(v, 23);
    vdp_build_line(v, row);
    static const uint8_t six[6] = {1, 2, 3, 4, 5, 6};
    CHECK(memcmp(row + 40, six, 6) == 0);
    poke(v, L0_ATTRS, FLIP_X);
    vdp_line_start(v, 23);
    vdp_build_line(v, row);
    static const uint8_t six_flipped[6] = {6, 5, 4, 3, 2, 1};
    CHECK(memcmp(row + 40, six_flipped, 6) == 0);
    CHECK_EQ(1, row[46]);                   // cell 1, unflipped, starts at pattern pixel 0
    free(v);
}

// §8: attribute b7 is bit 8 of the pattern index at 2 and 4bpp — tiles 256 to
// 511 — and ignored at 8bpp, as is the sub-palette.
TEST(pattern_bit_8_at_2_and_4bpp_only) {
    vdp_t *v = full();
    uint8_t row[VDP_WIDTH];
    poke(v, L0_NAMES, 3);
    poke(v, L0_ATTRS, BIT8 | 0x01);

    set_reg(v, L0CTRL, ENABLE | OPAQUE | PER_CELL | BPP2);
    poke(v, (uint16_t)(L0_PATTERNS + 3 * 16), 0x40);        // tile 3: value 1 first
    poke(v, (uint16_t)(L0_PATTERNS + 259 * 16), 0xc0);      // tile 259: value 3 first
    line_of(v, 0, row);
    CHECK_EQ(4 + 3, row[0]);

    set_reg(v, L0CTRL, ENABLE | OPAQUE | PER_CELL | BPP4);
    poke(v, (uint16_t)(L0_PATTERNS + 3 * 32), 0x10);
    poke(v, (uint16_t)(L0_PATTERNS + 259 * 32), 0x90);
    line_of(v, 0, row);
    CHECK_EQ(0x19, row[0]);

    set_reg(v, L0CTRL, ENABLE | OPAQUE | PER_CELL | BPP8);
    poke(v, (uint16_t)(L0_PATTERNS + 3 * 64), 0x42);
    line_of(v, 0, row);
    CHECK_EQ(0x42, row[0]);                 // tile 3, index as stored
    free(v);
}

// §8's attribute source "none" at 2, 4 and 8bpp: a constant byte with no flip,
// no priority and no ninth bit — sub-palette 0 in LxPAL's quarter at 2bpp,
// palette row LxPAL at 4bpp, the value itself at 8bpp. A per-cell table full of
// flips and priority bits changes nothing.
TEST(attribute_source_none_at_every_depth) {
    vdp_t *v = full();
    uint8_t row[VDP_WIDTH];
    set_reg(v, L0PAL, 0x06);
    for (unsigned a = 0; a < 16; a++) poke(v, (uint16_t)(L0_ATTRS + a), 0xff);
    poke(v, L0_PATTERNS, 0x40);                              // 2bpp: 1 0 0 0
    set_reg(v, L0CTRL, ENABLE | OPAQUE | NONE | BPP2);
    line_of(v, 0, row);
    CHECK_EQ(0x80 + 1, row[0]);                              // (6 & 3) x 64 + 1
    CHECK_EQ(0x80, row[1]);

    poke(v, L0_PATTERNS, 0x70);                              // 4bpp: 7 0
    set_reg(v, L0CTRL, ENABLE | OPAQUE | NONE | BPP4);
    line_of(v, 0, row);
    CHECK_EQ(0x67, row[0]);
    CHECK_EQ(0x60, row[1]);

    poke(v, L0_PATTERNS, 0x70);
    set_reg(v, L0CTRL, ENABLE | OPAQUE | NONE | BPP8);
    line_of(v, 0, row);
    CHECK_EQ(0x70, row[0]);
    free(v);
}

// §8: per pattern group and per pattern row still fetch an attribute byte at
// 4bpp. The group byte is found by the 8-bit name, so its ninth bit moves all
// eight names of the group; the row byte by the pixel row before any flip.
TEST(group_and_row_attributes_at_4bpp) {
    vdp_t *v = full();
    uint8_t row[VDP_WIDTH];
    poke(v, L0_NAMES, 0x0a);                                 // group 1
    poke(v, (uint16_t)(L0_NAMES + 1), 0x0f);                 // group 1
    poke(v, (uint16_t)(L0_NAMES + 2), 0x10);                 // group 2
    poke(v, (uint16_t)(L0_ATTRS + 1), BIT8 | 0x03);
    poke(v, (uint16_t)(L0_ATTRS + 2), 0x04);
    poke(v, (uint16_t)(L0_PATTERNS + 0x10a * 32), 0x10);
    poke(v, (uint16_t)(L0_PATTERNS + 0x10f * 32), 0x20);
    poke(v, (uint16_t)(L0_PATTERNS + 0x010 * 32), 0x30);
    set_reg(v, L0CTRL, ENABLE | OPAQUE | PER_GROUP | BPP4);
    line_of(v, 0, row);
    CHECK_EQ(0x31, row[0]);
    CHECK_EQ(0x32, row[8]);
    CHECK_EQ(0x43, row[16]);

    // Per row: tile 5's row 0 attribute flips it top to bottom, so display line
    // 0 draws pattern row 7 in row 0's colours; line 7 draws row 7 in its own.
    poke(v, L0_NAMES, 5);
    poke(v, (uint16_t)(L0_ATTRS + 5 * 8 + 0), FLIP_Y | 0x09);
    poke(v, (uint16_t)(L0_ATTRS + 5 * 8 + 7), 0x0c);
    poke(v, (uint16_t)(L0_PATTERNS + 5 * 32 + 0 * 4), 0x10);
    poke(v, (uint16_t)(L0_PATTERNS + 5 * 32 + 7 * 4), 0x70);
    set_reg(v, L0CTRL, ENABLE | OPAQUE | PER_ROW | BPP4);
    line_of(v, 0, row);
    CHECK_EQ(0x97, row[0]);
    line_of(v, 7, row);
    CHECK_EQ(0xc7, row[0]);
    free(v);
}

// §12's seven levels, one cell apiece. Layer 0 at 4bpp, tile 1 solid in
// sub-palette 1 (index $13); layer 1 at 4bpp, tile 1 solid in sub-palette 2
// ($25), tile 2 transparent; 8 x 8 1bpp sprites in sub-palette 14 (index 29).
TEST(all_seven_levels) {
    vdp_t *v = full();
    uint8_t row[VDP_WIDTH];
    set_reg(v, L0CTRL, ENABLE | PER_CELL | BPP4);
    set_reg(v, L1CTRL, ENABLE | PER_CELL | BPP4);
    for (unsigned r = 0; r < 8; r++) {
        for (unsigned b = 0; b < 4; b++) {
            poke(v, (uint16_t)(L0_PATTERNS + 32 + 4 * r + b), 0x33);
            poke(v, (uint16_t)(L1_PATTERNS + 32 + 4 * r + b), 0x55);
        }
    }
    enum { L0 = 0x13, L1 = 0x25, SPRITE = 29, L0N = 0x01, L0F = 0x41, L1N = 0x02, L1F = 0x42, SN = 0x0e, SF = 0x4e };
    // Each cell's layer 0 attribute, layer 1 tile and attribute, and sprite
    // attribute; what it shows with the sprite on the line, and without.
    static const struct { uint8_t l0, l1_tile, l1, sprite, with, without; } cells[] = {
        {L0N, 1, L1N, 0, L1, L1},               // 3 over 1
        {L0F, 1, L1N, 0, L0, L0},               // 4 over 3
        {L0F, 1, L1F, 0, L1, L1},               // 6 over 4
        {L0N, 2, L1N, 0, L0, L0},               // a transparent layer 1 pixel: 1
        {L0F, 2, L1F, 0, L0, L0},               // and 4
        {L0N, 2, L1N, SN, SPRITE, L0},          // 2 over 1
        {L0F, 2, L1N, SN, L0, L0},              // 4 over 2
        {L0F, 2, L1N, SF, SPRITE, L0},          // 5 over 4
        {L0N, 1, L1N, SF, SPRITE, L1},          // 5 over 3
        {L0N, 1, L1F, SF, L1, L1},              // 6 over 5
        {L0N, 1, L1N, SN, L1, L1},              // 3 over 2
        {0, 0, 0, 0, BACKDROP, BACKDROP},       // 0: tile 0 is transparent in both
    };
    enum { CELLS = sizeof cells / sizeof cells[0] };
    set_reg(v, SPRATTR, 0xc0);              // slots at $6000
    set_reg(v, SPRPAT, 0x0e);               // patterns at $7000
    for (unsigned r = 0; r < 8; r++) poke(v, (uint16_t)(0x7000 + r), 0xff);
    unsigned slots = 0;
    for (unsigned c = 0; c < CELLS; c++) {
        poke(v, (uint16_t)(L0_NAMES + c), cells[c].l0 ? 1 : 0);
        poke(v, (uint16_t)(L0_ATTRS + c), cells[c].l0);
        poke(v, (uint16_t)(L1_NAMES + c), cells[c].l1_tile);
        poke(v, (uint16_t)(L1_ATTRS + c), cells[c].l1);
        if (!cells[c].sprite) continue;
        uint16_t at = (uint16_t)(0x6000 + 4 * slots++);
        poke(v, at, 16);                    // Y 16: display lines 16-23, not 0-7
        poke(v, (uint16_t)(at + 1), (uint8_t)(8 * c));
        poke(v, (uint16_t)(at + 3), cells[c].sprite);
    }
    poke(v, (uint16_t)(0x6000 + 4 * slots), 0xd0);
    set_reg(v, SPRCTRL, 0x05);              // on, $D0 terminator, 1bpp, no collision

    // Display line 0 has no sprites: the build keeps levels for the layers'
    // own contest, because layer 0 can carry b6.
    line_of(v, 0, row);
    CHECK_EQ(0, v->sprite_count);
    for (unsigned c = 0; c < CELLS; c++) CHECK(all(row, 8 * c, 8 * c + 8, cells[c].without));

    // Cell row 2 (display line 16) names the same tiles, and has the sprites.
    for (unsigned c = 0; c < CELLS; c++) {
        poke(v, (uint16_t)(L0_NAMES + 80 + c), cells[c].l0 ? 1 : 0);
        poke(v, (uint16_t)(L0_ATTRS + 80 + c), cells[c].l0);
        poke(v, (uint16_t)(L1_NAMES + 80 + c), cells[c].l1_tile);
        poke(v, (uint16_t)(L1_ATTRS + 80 + c), cells[c].l1);
    }
    line_of(v, 16, row);
    CHECK_EQ(slots, v->sprite_count);
    for (unsigned c = 0; c < CELLS; c++) CHECK(all(row, 8 * c, 8 * c + 8, cells[c].with));

    // A 1bpp layer 1 has no b6 (§8): level 3, under layer 0's level 4, over its
    // 1. Opaque, coloured by COLOR, whose background nibble is 14: index $6E.
    set_reg(v, L1CTRL, ENABLE | OPAQUE | NONE | BPP1);
    set_reg(v, L1PAL, 0x06);
    line_of(v, 0, row);
    CHECK(all(row, 0, 8, 0x6e));
    CHECK(all(row, 8, 16, L0));
    CHECK(all(row, 24, 32, 0x6e));
    free(v);
}

// §12: every line's contest starts from the backdrop. A layer 0 cell at level
// 4 on one line leaves nothing behind for the next, where layer 0 is
// transparent and layer 1's ordinary cell draws — with no sprites on either.
TEST(each_line_starts_from_the_backdrop) {
    vdp_t *v = full();
    uint8_t row[VDP_WIDTH];
    set_reg(v, L0CTRL, ENABLE | PER_CELL | BPP4);
    set_reg(v, L1CTRL, ENABLE | PER_CELL | BPP4);
    for (unsigned b = 0; b < 4; b++) {
        poke(v, (uint16_t)(L0_PATTERNS + 32 + b), 0x33);          // layer 0 tile 1, row 0
        poke(v, (uint16_t)(L1_PATTERNS + 32 + 4 + b), 0x55);      // layer 1 tile 1, row 1
    }
    poke(v, L0_NAMES, 1);
    poke(v, L0_ATTRS, PRIORITY | 0x01);
    poke(v, L1_NAMES, 1);
    poke(v, L1_ATTRS, 0x02);

    line_of(v, 0, row);                     // layer 0 at level 4; layer 1's row 0 is clear
    CHECK(all(row, 0, 8, 0x13));
    line_of(v, 1, row);                     // layer 0's row 1 is clear; layer 1 at level 3
    CHECK(all(row, 0, 8, 0x25));
    free(v);
}

// §5, §7: pattern addresses wrap at 64 KB at every depth. At 8bpp from $F800,
// tile $1F's row 7 ends at $FFFF and tile $20 starts at $0000; at 4bpp tile
// $1FF, by bit 8, is at $F800 + $3FE0 = $37E0.
TEST(pattern_addresses_wrap_at_64kb) {
    vdp_t *v = full();
    uint8_t row[VDP_WIDTH];
    set_reg(v, L0NAME, 0x08);               // names at $2000, clear of $0000
    set_reg(v, L0ATTR, 0x0c);               // attributes at $3000
    set_reg(v, L0PAT, 0xff);
    set_reg(v, L0CTRL, ENABLE | OPAQUE | PER_CELL | BPP8);
    poke(v, 0x2000, 0x1f);
    poke(v, 0x2001, 0x20);
    poke(v, 0xffff, 0x77);
    poke(v, 0x0000, 0x88);
    line_of(v, 7, row);
    CHECK_EQ(0x77, row[7]);
    line_of(v, 0, row);
    CHECK_EQ(0x88, row[8]);

    set_reg(v, L0CTRL, ENABLE | OPAQUE | PER_CELL | BPP4);
    poke(v, 0x2000, 0xff);
    poke(v, 0x3000, BIT8 | 0x05);
    poke(v, 0x37e0, 0xa0);
    line_of(v, 0, row);
    CHECK_EQ(0x5a, row[0]);
    free(v);
}

// §13 at depth: per-pixel scroll in both axes, nine bits of X, the same walk
// as 1bpp's with flips and priority in it — and layer 1 scrolled on its own.
TEST(layers_scroll_independently_at_depth) {
    vdp_t *v = full();
    uint8_t row[VDP_WIDTH];
    set_reg(v, L0CTRL, ENABLE | OPAQUE | PER_CELL | BPP8 | SCRX_BIT8);
    set_reg(v, L0SCRX, 0x05);               // 256 + 5 = 261: cell 32, pixel 5
    set_reg(v, L0SCRY, 3);                  // display line 0 is map row 3
    poke(v, (uint16_t)(L0_NAMES + 32), 2);
    for (unsigned x = 0; x < 8; x++) poke(v, (uint16_t)(L0_PATTERNS + 2 * 64 + 3 * 8 + x), (uint8_t)(0x40 + x));
    line_of(v, 0, row);
    CHECK_EQ(0x45, row[0]);
    CHECK_EQ(0x47, row[2]);
    CHECK_EQ(0x00, row[3]);                 // cell 33: tile 0

    // Layer 1, unscrolled, transparent but for one pixel of cell 0, over it.
    set_reg(v, L1CTRL, ENABLE | PER_CELL | BPP4);
    poke(v, (uint16_t)(L1_PATTERNS + 1), 0x01);  // pixel 3
    line_of(v, 0, row);
    CHECK_EQ(0x45, row[0]);
    CHECK_EQ(0x01, row[3]);
    free(v);
}

int main(void) {
    RUN(each_depth_decodes_and_maps_its_palette);
    RUN(value_0_is_transparent_unless_index_0_is_opaque);
    RUN(the_4bpp_table_is_the_mapping);
    RUN(flips_mirror_the_cell);
    RUN(pattern_bit_8_at_2_and_4bpp_only);
    RUN(attribute_source_none_at_every_depth);
    RUN(group_and_row_attributes_at_4bpp);
    RUN(all_seven_levels);
    RUN(each_line_starts_from_the_backdrop);
    RUN(pattern_addresses_wrap_at_64kb);
    RUN(layers_scroll_independently_at_depth);
    return TEST_RESULT();
}
