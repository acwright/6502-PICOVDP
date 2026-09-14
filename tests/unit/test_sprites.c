// §10's sprites, §9's legacy sprites and §12 against the layers, at the core's
// own interface: the latch evaluates, and the build draws on two cores.
//
// Video.test.ts covers the sprites through the adapter, which builds each row
// on one thread the instant its latch falls. What it cannot see is here: that
// every division of a line between the cores gives the same row and the same
// status, that core 0's half publishes nothing until it is merged, that an
// overflow is published at the latch and a collision only by the build, that a
// write during the build does not reach it, and that the sprite tables wrap at
// 64 KB.

#include <string.h>

#include "card.h"
#include "test.h"

enum {
    MODE1 = 0x01, COLOR = 0x07, IRQEN = 0x0a, VMODE = 0x0d, L0CTRL = 0x15, L0PAT = 0x12, L0ATTR = 0x11, L0SCRX = 0x13,
    L1SCRY = 0x1c,
    L1NAME = 0x18, L1ATTR = 0x19, L1PAT = 0x1a, L1CTRL = 0x1d, L1PAL = 0x1e, SPRATTR = 0x20, SPRPAT = 0x21,
    SPRCOUNT = 0x22, SPRCTRL = 0x23, SPRLIMIT = 0x24, SPRPAL = 0x25,
};

enum { DISP = 0x40, M1 = 0x10, SIZE16 = 0x02, MAG = 0x01, ENABLE = 0x10, OPAQUE = 0x20 };

// SPRCTRL: enabled, collision, detailed, and a depth in b5:4.
enum { SPR_ON = 0x01, SPR_COLLIDE = 0x02, SPR_TERMINATE = 0x04, SPR_DETAILED = 0x08 };

static void poke(vdp_t *v, uint16_t address, uint8_t value) {
    vdp_debug_set_vram(v, address, value);
}

static void slot(vdp_t *v, uint16_t table, unsigned n, uint8_t y, uint8_t x, uint8_t pattern, uint8_t attributes) {
    uint16_t at = (uint16_t)(table + 4 * n);
    poke(v, at, y);
    poke(v, (uint16_t)(at + 1), x);
    poke(v, (uint16_t)(at + 2), pattern);
    poke(v, (uint16_t)(at + 3), attributes);
}

static void row_after(vdp_t *v, uint16_t latch, uint8_t *row) {
    vdp_line_start(v, latch);
    vdp_build_line(v, row);
}

static bool all(const uint8_t *row, unsigned from, unsigned to, uint8_t index) {
    for (unsigned x = from; x < to; x++) {
        if (row[x] != index) return false;
    }
    return true;
}

// Graphics, VMODE $3: 256 x 240 at x 32, no top border, so display line n is
// screen line n. Layer 0 at 1bpp with per-cell attributes of zero draws
// nothing; the backdrop is 0. Sprites at 1bpp in group SPRPAL 0, sub-palette
// 7: a pixel of value 1 is index 15. Patterns at $4000, slots at $0000.
static vdp_t *graphics(void) {
    vdp_t *v = new_card();
    set_reg(v, MODE1, DISP);
    set_reg(v, VMODE, 0x03);
    set_reg(v, COLOR, 0x00);
    set_reg(v, L0CTRL, ENABLE);
    set_reg(v, L0ATTR, 0x08);               // $2000, all zero
    set_reg(v, L0PAT, 0x0c);                // $6000
    set_reg(v, SPRPAT, 0x08);               // $4000
    set_reg(v, SPRCTRL, SPR_ON | SPR_COLLIDE);  // 1bpp, no terminator
    for (unsigned r = 0; r < 8; r++) poke(v, (uint16_t)(0x4000 + r), 0xff);  // pattern 0: solid
    return v;
}

// ---- the division of a line between the cores ----

/** mulberry32, as tools/fuzz.mjs. */
static uint32_t rng_state;
static uint32_t rng(void) {
    uint32_t t = (rng_state += 0x6d2b79f5u);
    t = (t ^ (t >> 15)) * (t | 1);
    t ^= t + (t ^ (t >> 7)) * (t | 61);
    return t ^ (t >> 14);
}
static unsigned below(unsigned n) {
    return rng() % n;
}

// A random scene: a mode, sprite size, depth, collision and limits, layers at
// every depth and attribute source over random tables, scrolled — so at every
// §12 level from Phase 7 — and slots placed around the lines the test builds.
static void scene(vdp_t *v, uint16_t around) {
    static const uint8_t vmodes[] = {0x0, 0x1, 0x2, 0x3, 0x4};
    uint8_t vmode = vmodes[below(5)];
    set_reg(v, VMODE, vmode);
    set_reg(v, MODE1, (uint8_t)(DISP | below(4) | (vmode == 0 && below(8) == 0 ? M1 : 0)));
    set_reg(v, SPRCTRL, (uint8_t)(SPR_ON | (below(4) ? SPR_COLLIDE : 0) | (below(2) ? SPR_DETAILED : 0) | (below(4) << 4)));
    set_reg(v, SPRCOUNT, (uint8_t)(below(4) ? 64 : below(70)));
    set_reg(v, SPRLIMIT, (uint8_t)(below(4) ? 32 : below(40)));
    set_reg(v, SPRPAL, (uint8_t)rng());
    set_reg(v, SPRATTR, 0x00);
    set_reg(v, SPRPAT, (uint8_t)rng());
    set_reg(v, L0CTRL, (uint8_t)(ENABLE | (below(2) ? OPAQUE : 0) | (below(4) << 2) | below(4)));
    set_reg(v, L0SCRX, (uint8_t)rng());
    set_reg(v, L0ATTR, (uint8_t)rng());
    set_reg(v, L0PAT, (uint8_t)rng());
    set_reg(v, L1CTRL, (uint8_t)((below(2) ? ENABLE : 0) | (below(4) ? 0 : OPAQUE) | (below(4) << 2) | below(4)));
    set_reg(v, L1SCRY, (uint8_t)rng());
    set_reg(v, L1NAME, (uint8_t)rng());
    set_reg(v, L1ATTR, (uint8_t)rng());
    set_reg(v, L1PAT, (uint8_t)rng());
    set_reg(v, 0x0c, 0x3f);

    for (unsigned a = 0; a < 0xfc00; a++) {
        unsigned roll = below(4);
        poke(v, (uint16_t)a, roll == 0 ? 0x00 : roll == 1 ? 0xff : (uint8_t)rng());
    }
    // Slots whose tops fall in the 33 lines up to `around`, most of them on the picture.
    for (unsigned n = 0; n < 64; n++) {
        int y = (int)around - (int)below(34);
        uint8_t x = below(4) ? (uint8_t)below(256) : (uint8_t)rng();
        slot(v, 0x0000, n, (uint8_t)(y & 0xff), x, (uint8_t)rng(), (uint8_t)rng());
    }
}

// §6: what the build publishes, and what the latch did.
static bool same_status(const vdp_t *a, const vdp_t *b) {
    return a->stat0 == b->stat0 && a->irq_latch == b->irq_latch && a->frame_events == b->frame_events &&
           a->overflow_sprite == b->overflow_sprite && memcmp(a->collision_map, b->collision_map, 8) == 0;
}

// A line built with core 0 taking picture columns [0, split).
static void build_at(vdp_t *v, int split, uint8_t *row) {
    vdp_sprline_t core0;
    vdp_build_sprites(v, &core0, 0, split);
    vdp_build_layers(v, row);
    vdp_draw_sprites(v, row, split, VDP_WIDTH);
    vdp_merge_sprites(v, row, &core0);
}

// PLAN.md section 3: priority among sprites and collision are exact within
// each core's columns, so every split — the 32-pixel boundaries the firmware
// uses, and any column at all — builds the same row and publishes the same
// status as core 1 building the whole line.
TEST(every_split_builds_the_same_line) {
    vdp_t *v = new_card();
    vdp_t *w = malloc(sizeof *w);
    uint8_t whole[VDP_WIDTH], split_row[VDP_WIDTH];
    unsigned lines = 0, drawn = 0, collided = 0, overflowed = 0, mapped = 0;
    rng_state = 6;

    for (unsigned n = 0; n < 60; n++) {
        uint16_t around = (uint16_t)below(250);
        scene(v, around);
        for (int line = (int)around - 34; line <= (int)around + 2; line += 3) {
            uint16_t latch = (uint16_t)((line + VDP_SCREEN_LINES - 1) % VDP_SCREEN_LINES);
            v->stat0 = 0;
            memset(v->collision_map, 0, 8);
            v->frame_events = 0;
            vdp_line_start(v, latch);
            memcpy(w, v, sizeof *w);
            build_at(v, 0, whole);
            lines++;
            if (v->sprite_count) drawn++;
            if (v->stat0 & 0x20) collided++;
            if (v->stat0 & 0x40) overflowed++;
            for (unsigned b = 0; b < 8; b++) {
                if (v->collision_map[b]) {
                    mapped++;
                    break;
                }
            }

            static const int splits[] = {32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 1, 7, 100, 255, 319};
            for (unsigned s = 0; s < sizeof splits / sizeof splits[0]; s++) {
                vdp_t *u = malloc(sizeof *u);
                memcpy(u, w, sizeof *u);
                build_at(u, splits[s], split_row);
                CHECK(memcmp(whole, split_row, VDP_WIDTH) == 0);
                CHECK(same_status(v, u));
                free(u);
            }
            // And vdp_build_line, at whatever vdp_split_choose picks.
            vdp_build_line(w, split_row);
            CHECK(memcmp(whole, split_row, VDP_WIDTH) == 0);
            CHECK(same_status(v, w));
            memcpy(v, w, sizeof *v);
        }
    }
    // The scenes reach what they are for.
    printf("  %u lines: sprites on %u, collision on %u, overflow on %u, a detailed map on %u\n", lines, drawn,
           collided, overflowed, mapped);
    CHECK(drawn > lines * 2 / 5);
    CHECK(collided > lines / 4);
    CHECK(overflowed > lines / 20);
    CHECK(mapped > lines / 8);
    free(v);
    free(w);
}

// Core 0 writes no status (PLAN.md section 3): what its half found waits in the
// sprite line until core 1 merges it.
TEST(core_0s_collisions_wait_for_the_merge) {
    vdp_t *v = graphics();
    uint8_t row[VDP_WIDTH];
    set_reg(v, SPRCTRL, SPR_ON | SPR_COLLIDE | SPR_DETAILED);
    set_reg(v, IRQEN, 0x08);
    slot(v, 0x0000, 0, 10, 20, 0, 0x07);
    slot(v, 0x0000, 1, 10, 24, 0, 0x07);    // overlaps slot 0 at x 24-27

    vdp_line_start(v, 10);
    vdp_sprline_t core0;
    vdp_build_sprites(v, &core0, 0, 32);
    CHECK(core0.collided);
    CHECK_EQ(0x03, core0.collisions);
    CHECK_EQ(0, v->stat0);
    CHECK(!vdp_int_asserted(v));

    vdp_build_layers(v, row);
    vdp_draw_sprites(v, row, 32, VDP_WIDTH);
    CHECK_EQ(0, v->stat0);                  // core 1's half had nothing to collide
    CHECK(all(row, 32 + 20, 32 + 32, 0));   // nor has core 0's half been drawn yet

    vdp_merge_sprites(v, row, &core0);
    CHECK_EQ(0x20, v->stat0);
    CHECK_EQ(0x03, v->collision_map[0]);
    CHECK(vdp_int_asserted(v));
    CHECK(all(row, 32 + 20, 32 + 32, 15));
    free(v);
}

// §14: an overflow is the latch's — published as the line's sprites are
// evaluated, before a pixel of it is built — and a collision the build's.
TEST(overflow_at_the_latch_collision_in_the_build) {
    vdp_t *v = graphics();
    uint8_t row[VDP_WIDTH];
    set_reg(v, SPRLIMIT, 2);
    set_reg(v, IRQEN, 0x0c);
    for (unsigned n = 0; n < 3; n++) slot(v, 0x0000, n, 50, 100, 0, 0x07);

    vdp_line_start(v, 50);                  // builds display line 51
    CHECK_EQ(0x42, v->stat0);               // OVF, sprite 2
    CHECK_EQ(2, v->overflow_sprite);
    CHECK_EQ(0x04, vdp_debug_status(v, 1));
    vdp_build_line(v, row);
    CHECK_EQ(0x62, v->stat0);               // the two drawn collide
    CHECK_EQ(0x0c, vdp_debug_status(v, 1));

    // Display line 50 has none: the sprites' first row is line 50, built at 49.
    vdp_t *w = graphics();
    for (unsigned n = 0; n < 3; n++) slot(w, 0x0000, n, 50, 100, 0, 0x07);
    set_reg(w, SPRLIMIT, 2);
    vdp_line_start(w, 48);
    CHECK_EQ(0, w->stat0);
    vdp_line_start(w, 49);
    CHECK_EQ(0x42, w->stat0);
    free(v);
    free(w);
}

// §3, §10: no sprite is evaluated off the picture, with the display off, or in
// legacy Text — so none overflows or collides — and a sprite never draws in
// the border.
TEST(no_sprites_outside_the_picture) {
    vdp_t *v = new_card();
    uint8_t row[VDP_WIDTH];
    set_reg(v, MODE1, DISP);
    set_reg(v, VMODE, 0x02);                // Compact: 256 x 192 at (32, 24)
    set_reg(v, SPRPAT, 0x08);
    set_reg(v, SPRLIMIT, 1);
    for (unsigned r = 0; r < 8; r++) poke(v, (uint16_t)(0x4000 + r), 0xff);

    // Every slot at Y 186 and X 504, which is -8: wholly off the picture's left,
    // and covering display lines 186-193, of which 186-191 are the picture's.
    for (unsigned n = 0; n < 32; n++) slot(v, 0x0000, n, 186, 0xf8, 0, 0x87);
    for (unsigned latch = 0; latch < VDP_SCREEN_LINES; latch++) {
        row_after(v, (uint16_t)latch, row);
        unsigned display = (latch + 1 + VDP_SCREEN_LINES - 24) % VDP_SCREEN_LINES;
        bool covered = display >= 186 && display < 192;
        CHECK_EQ(covered ? 0x40 : 0, v->stat0 & 0x40);  // counted, though wholly off the left
        CHECK(all(row, 0, VDP_WIDTH, row[0]));
        vdp_read(v, 1);                     // acknowledge
    }

    // A sprite at X 250 draws picture columns 250-255 and no border.
    slot(v, 0x0000, 0, 100, 250, 0, 0x07);
    slot(v, 0x0000, 1, 0xd0, 0, 0, 0);
    set_reg(v, SPRCTRL, SPR_ON | SPR_COLLIDE | SPR_TERMINATE);
    row_after(v, 124, row);                 // display line 101
    CHECK(all(row, 32 + 250, 32 + 256, 15));
    CHECK(all(row, 32 + 256, VDP_WIDTH, 0));

    // Display off, and legacy Text: nothing evaluated.
    set_reg(v, SPRLIMIT, 0);                // would overflow on the first sprite covering
    set_reg(v, MODE1, 0x00);
    vdp_line_start(v, 124);
    CHECK_EQ(0, v->stat0);
    CHECK_EQ(0, v->sprite_count);
    set_reg(v, VMODE, 0x00);
    set_reg(v, MODE1, DISP | M1);
    vdp_line_start(v, 124);
    CHECK_EQ(0, v->stat0);
    set_reg(v, VMODE, 0x01);                // VMODE's Text has sprites
    vdp_line_start(v, 124);
    CHECK_EQ(0x40, v->stat0);
    free(v);
}

// §12 at 1bpp: a sprite is over layer 0 (level 2 over 1) and under layer 1
// (3), unless its b6 lifts it to 5. A transparent layer 1 pixel shows it. A
// legacy sprite ignores b6. And the lowest slot owns the pixel even where it
// loses it to layer 1.
TEST(sprites_stand_between_the_layers) {
    vdp_t *v = graphics();
    uint8_t row[VDP_WIDTH];
    // Layer 0: cell 0 solid, COLOR's pair; layer 1: cell 0 ..XXXXXX in fg 9.
    set_reg(v, L0CTRL, ENABLE | (3 << 2));
    set_reg(v, COLOR, 0x30);
    poke(v, 0x6000, 0xff);                  // layer 0 pattern 0 row 0 (name table at $0000 names 0)
    set_reg(v, L1CTRL, ENABLE | (0 << 2));
    set_reg(v, L1NAME, 0x20);               // $8000
    set_reg(v, L1ATTR, 0x24);               // $9000
    set_reg(v, L1PAT, 0x14);                // $A000
    poke(v, 0x8000, 1);
    poke(v, 0x9000, 0x90);
    poke(v, 0xa000 + 8, 0x3f);
    set_reg(v, SPRATTR, 0x60);              // slots at $3000, clear of the name tables
    slot(v, 0x3000, 0, 0, 0, 0, 0x07);      // over the cell, level 2
    slot(v, 0x3000, 1, 0xd0, 0, 0, 0);
    set_reg(v, SPRCTRL, SPR_ON | SPR_COLLIDE | SPR_TERMINATE);

    row_after(v, 261, row);
    static const uint8_t level2[8] = {15, 15, 9, 9, 9, 9, 9, 9};
    CHECK(memcmp(row + 32, level2, 8) == 0);
    CHECK_EQ(3, row[32 + 8]);               // beside it, layer 0

    slot(v, 0x3000, 0, 0, 0, 0, 0x47);      // b6: level 5
    row_after(v, 261, row);
    CHECK(all(row, 32, 40, 15));

    // Slot 0 ordinary, slot 1 in front: slot 0 owns the pixel, and loses it to
    // layer 1 with slot 1 behind it.
    slot(v, 0x3000, 0, 0, 0, 0, 0x07);
    slot(v, 0x3000, 1, 0, 0, 0, 0x4a);
    slot(v, 0x3000, 2, 0xd0, 0, 0, 0);
    row_after(v, 261, row);
    CHECK(memcmp(row + 32, level2, 8) == 0);

    // An opaque layer 1 (LxCTRL b5) hides an ordinary sprite everywhere (§12).
    set_reg(v, L1CTRL, ENABLE | OPAQUE);
    row_after(v, 261, row);
    static const uint8_t occluded[8] = {0, 0, 9, 9, 9, 9, 9, 9};
    CHECK(memcmp(row + 32, occluded, 8) == 0);
    set_reg(v, L1CTRL, ENABLE);

    // The legacy submode: b6 ignored, the colour is b3:0 itself. Compact's
    // origin, and legacy Y: $FF puts the first row on display line 0.
    set_reg(v, VMODE, 0x00);
    set_reg(v, 0x05, 0x60);                 // SPRATTR through its alias
    slot(v, 0x3000, 0, 0xff, 0, 0, 0x4a);
    slot(v, 0x3000, 1, 0xd0, 0, 0, 0);
    row_after(v, 23, row);
    static const uint8_t legacy[8] = {10, 10, 9, 9, 9, 9, 9, 9};
    CHECK(memcmp(row + 32, legacy, 8) == 0);
    free(v);
}

// §9: the legacy submode reads Y as the TMS9918 did — the first row on the line
// after Y, $E1-$FF as -31…-1 — and always ends the list at $D0. VMODE's modes
// read Y as the top edge, 241-255 as -15…-1, and $D0 only while SPRCTRL b2 is set.
TEST(legacy_and_vmode_read_y_differently) {
    vdp_t *v = new_card();
    uint8_t row[VDP_WIDTH];
    set_reg(v, MODE1, DISP);                // legacy Graphics I: Compact, origin (32, 24)
    set_reg(v, 0x03, 0x80);                 // L0ATTR x $40: $2000, zero, so layer 0 is transparent
    set_reg(v, 0x06, 0x08);                 // SPRPAT: $4000
    set_reg(v, SPRCTRL, SPR_ON);            // b2 clear: legacy forces it anyway
    for (unsigned r = 0; r < 8; r++) poke(v, (uint16_t)(0x4000 + 8 + r), (uint8_t)(0x80 >> r));  // pattern 1: a diagonal
    slot(v, 0x0000, 0, 9, 0, 1, 0x0f);
    slot(v, 0x0000, 1, 0xd0, 0, 0, 0);
    slot(v, 0x0000, 2, 9, 100, 1, 0x0f);    // past the terminator

    row_after(v, 24 + 9 - 1, row);          // display line 9: nothing
    CHECK(all(row, 0, VDP_WIDTH, 0));
    row_after(v, 24 + 10 - 1, row);         // display line 10: row 0
    CHECK_EQ(15, row[32]);
    CHECK(all(row, 33, VDP_WIDTH, 0));      // and not slot 2
    slot(v, 0x0000, 0, 0xf9, 0, 1, 0x0f);   // -7 + 1: display line 0 is row 6
    row_after(v, 23, row);
    CHECK_EQ(15, row[32 + 6]);
    CHECK_EQ(1, v->sprite_count);

    set_reg(v, VMODE, 0x02);                // Compact through VMODE
    set_reg(v, L0CTRL, ENABLE);
    set_reg(v, L0ATTR, 0x08);               // $2000
    set_reg(v, SPRPAL, 0x00);
    slot(v, 0x0000, 0, 9, 0, 1, 0x07);
    row_after(v, 24 + 9 - 1, row);          // display line 9: row 0
    CHECK_EQ(15, row[32]);
    CHECK_EQ(2, v->sprite_count);           // b2 clear: $D0 is row 208, and slot 2 is drawn
    CHECK_EQ(31, row[32 + 100]);            // sub-palette 15 at 1bpp: 15 x 2 + 1
    slot(v, 0x0000, 0, 0xf9, 0, 1, 0x07);   // 249: -7, display line 0 is row 7
    row_after(v, 23, row);
    CHECK_EQ(15, row[32 + 7]);
    set_reg(v, SPRCTRL, SPR_ON | SPR_TERMINATE);
    row_after(v, 24 + 9 - 1, row);
    CHECK_EQ(0, v->sprite_count);           // slot 0 is at -7 now, and b2 ends the list at slot 1
    free(v);
}

// §10: a 16 x 16 sprite draws patterns N to N + 3 as top left, bottom left,
// top right and bottom right; b4 and b5 flip the whole sprite, quadrants and
// all; MODE1 b0 doubles every pixel and row.
TEST(quadrants_flips_and_magnification) {
    vdp_t *v = graphics();
    uint8_t row[VDP_WIDTH];
    set_reg(v, MODE1, DISP | SIZE16);
    // One pixel in row 0 and row 7 of each quadrant, at a column its own.
    static const uint8_t top[4] = {0x80, 0x40, 0x20, 0x10}, bottom[4] = {0x08, 0x04, 0x02, 0x01};
    for (unsigned q = 0; q < 4; q++) {
        poke(v, (uint16_t)(0x4000 + (8 + q) * 8), top[q]);
        poke(v, (uint16_t)(0x4000 + (8 + q) * 8 + 7), bottom[q]);
    }
    slot(v, 0x0000, 0, 40, 64, 8, 0x07);    // pattern 8: quadrants 8-11

    static const struct { uint8_t attributes; uint16_t latch; unsigned x[2]; } cases[] = {
        {0x07, 39, {0, 8 + 2}},             // sprite row 0: TL's row 0 pixel 0, TR's pixel 2
        {0x07, 46, {4, 8 + 6}},             // row 7: TL's row 7 pixel 4, TR's pixel 6
        {0x07, 47, {1, 8 + 3}},             // row 8: BL's row 0 pixel 1, BR's pixel 3
        {0x17, 39, {15 - 0, 15 - 10}},      // flipped across: mirrored columns
        {0x27, 39, {5, 8 + 7}},             // flipped down: sprite row 15, BL's and BR's row 7
    };
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        slot(v, 0x0000, 0, 40, 64, 8, cases[c].attributes);
        row_after(v, cases[c].latch, row);
        for (unsigned x = 0; x < 16; x++) {
            bool on = x == cases[c].x[0] || x == cases[c].x[1];
            CHECK_EQ(on ? 15 : 0, row[32 + 64 + x]);
        }
    }

    set_reg(v, MODE1, DISP | SIZE16 | MAG);
    slot(v, 0x0000, 0, 40, 64, 8, 0x07);
    row_after(v, 40, row);                  // line 41: still sprite row 0
    for (unsigned x = 0; x < 32; x++) CHECK_EQ(x < 2 || x == 20 || x == 21 ? 15 : 0, row[32 + 64 + x]);
    free(v);
}

// §6: STAT0's index field keeps the first sprite dropped since STAT0 was read;
// STAT7 follows the most recent overflowing line.
TEST(stat7_follows_the_last_overflowing_line) {
    vdp_t *v = graphics();
    set_reg(v, SPRLIMIT, 1);
    slot(v, 0x0000, 0, 10, 0, 0, 0x07);
    slot(v, 0x0000, 1, 10, 20, 0, 0x07);    // dropped on lines 10-17
    slot(v, 0x0000, 2, 30, 0, 0, 0x07);
    for (unsigned n = 3; n < 36; n++) slot(v, 0x0000, n, 0xc8, 0, 0, 0);  // off the picture
    slot(v, 0x0000, 36, 30, 40, 0, 0x07);   // dropped on lines 30-37
    set_reg(v, SPRCOUNT, 64);

    vdp_line_start(v, 10);
    CHECK_EQ(0x41, v->stat0);
    CHECK_EQ(1, v->overflow_sprite);
    vdp_line_start(v, 30);
    CHECK_EQ(0x41, v->stat0);               // the first, still
    CHECK_EQ(36, v->overflow_sprite);       // which STAT0's five bits could not name
    CHECK_EQ(36, vdp_debug_status(v, 7));
    free(v);
}

// §9: a legacy sprite's b7 is the early clock, 32 pixels left, and its colour
// is b3:0 in palette row 0 whatever SPRPAL holds. §10: outside it, the index
// is ((SPRPAL x 16 + subpal) x 2^bpp + value) & $FF, and b7 is X bit 8.
TEST(the_early_clock_and_the_palette_mapping) {
    vdp_t *v = new_card();
    uint8_t row[VDP_WIDTH];
    set_reg(v, MODE1, DISP);                // legacy Graphics I
    set_reg(v, 0x03, 0x80);                 // layer 0 transparent
    set_reg(v, 0x06, 0x08);                 // SPRPAT: $4000
    set_reg(v, SPRPAL, 0x05);
    poke(v, 0x4000 + 8, 0x80);              // pattern 1, row 0: pixel 0
    slot(v, 0x0000, 0, 0xff, 40, 1, 0x8c);  // early clock: X 8; colour 12
    slot(v, 0x0000, 1, 0xd0, 0, 0, 0);
    row_after(v, 23, row);
    CHECK_EQ(12, row[32 + 8]);
    CHECK(all(row, 32 + 9, VDP_WIDTH, 0));

    set_reg(v, VMODE, 0x02);
    set_reg(v, L0CTRL, ENABLE);
    set_reg(v, L0ATTR, 0x08);
    set_reg(v, SPRCTRL, SPR_ON | SPR_TERMINATE);  // 1bpp: ((5 x 16 + 12) x 2 + 1) & $FF = $B9
    slot(v, 0x0000, 0, 0, 40, 1, 0x8c);     // X 256 + 40, off the picture
    slot(v, 0x0000, 2, 0xd0, 0, 0, 0);
    slot(v, 0x0000, 1, 0, 200, 1, 0x0c);
    row_after(v, 23, row);
    CHECK_EQ(0xb9, row[32 + 200]);
    CHECK_EQ(1, v->sprite_count);

    set_reg(v, SPRCTRL, SPR_ON | SPR_TERMINATE | (1 << 4));  // 2bpp: (92 x 4 + 3) & $FF = $73
    poke(v, 0x4000 + 16, 0xc0);             // pattern 1 at 16 bytes, row 0: value 3 in pixel 0
    row_after(v, 23, row);
    CHECK_EQ(0x73, row[32 + 200]);
    set_reg(v, SPRCTRL, SPR_ON | SPR_TERMINATE | (2 << 4));  // 4bpp: (92 x 16 + 9) & $FF = $C9
    poke(v, 0x4000 + 32, 0x90);
    row_after(v, 23, row);
    CHECK_EQ(0xc9, row[32 + 200]);
    free(v);
}

// §9: a legacy sprite of colour 0 draws nothing, lets the sprite behind it
// show, and collides; and is split between the cores like any other.
TEST(a_legacy_colour_0_sprite_collides_unseen) {
    vdp_t *v = new_card();
    uint8_t row[VDP_WIDTH];
    set_reg(v, MODE1, DISP);                // legacy Graphics I
    set_reg(v, 0x06, 0x08);                 // SPRPAT through its alias: $4000
    set_reg(v, 0x03, 0x80);                 // L0ATTR x $40: $2000, zero, so layer 0 is transparent
    set_reg(v, SPRCTRL, SPR_ON | SPR_COLLIDE | SPR_DETAILED);
    for (unsigned r = 0; r < 8; r++) poke(v, (uint16_t)(0x4000 + 8 + r), 0xff);  // pattern 1
    slot(v, 0x0000, 0, 9, 28, 1, 0x00);     // colour 0, across the split at 32
    slot(v, 0x0000, 1, 9, 30, 1, 0x05);
    slot(v, 0x0000, 2, 0xd0, 0, 0, 0);

    vdp_line_start(v, 33);                  // display line 10: the sprites' first row
    vdp_t *w = malloc(sizeof *w);
    memcpy(w, v, sizeof *w);
    build_at(v, 32, row);
    CHECK(all(row, 32 + 28, 32 + 30, 0));   // the backdrop, COLOR 0
    CHECK(all(row, 32 + 30, 32 + 38, 5));
    CHECK_EQ(0x20, v->stat0);
    CHECK_EQ(0x03, v->collision_map[0]);
    uint8_t whole[VDP_WIDTH];
    build_at(w, 0, whole);
    CHECK(memcmp(row, whole, VDP_WIDTH) == 0);
    free(v);
    free(w);
}

// PLAN.md section 3: the build reads the sprites as the latch took them. A
// write to a slot, a pattern, SPRCTRL or MODE1 between the latch and the build
// waits for the next line.
TEST(a_write_during_the_build_waits_for_the_next_line) {
    vdp_t *v = graphics();
    uint8_t before[VDP_WIDTH], during[VDP_WIDTH], after[VDP_WIDTH];
    slot(v, 0x0000, 0, 20, 40, 0, 0x07);
    slot(v, 0x0000, 1, 20, 44, 0, 0x03);

    row_after(v, 20, before);
    CHECK(all(before, 32 + 40, 32 + 48, 15));

    vdp_line_start(v, 20);
    uint8_t moved[4] = {20, 100, 0, 0x02};
    store(v, 0x0000, moved, 4);             // slot 0
    uint8_t clear[8] = {0};
    store(v, 0x4000, clear, 8);             // pattern 0
    set_reg(v, SPRCTRL, 0x00);
    set_reg(v, MODE1, DISP | SIZE16 | MAG);
    uint8_t status = v->stat0;
    vdp_build_line(v, during);
    CHECK(memcmp(before, during, VDP_WIDTH) == 0);
    CHECK_EQ(status | 0x20, v->stat0);      // and collided as the latch saw it

    set_reg(v, SPRCTRL, SPR_ON);
    row_after(v, 20, after);                // pattern 0 is clear now
    CHECK(all(after, 0, VDP_WIDTH, 0));
    free(v);
}

// §5, §7, §10: SPRPAT x $800 plus the pattern's offset wraps at 64 KB; SPRATTR
// x $80 reaches $7F80.
TEST(the_sprite_tables_wrap_at_64kb) {
    vdp_t *v = graphics();
    uint8_t row[VDP_WIDTH];
    set_reg(v, SPRCTRL, SPR_ON | (3 << 4));  // 8bpp
    set_reg(v, SPRCOUNT, 64);
    set_reg(v, SPRPAT, 0xff);               // $F800: pattern $20 at 64 bytes each is $10000
    set_reg(v, SPRATTR, 0xff);              // $7F80
    set_reg(v, 0x0c, 0x30);                 // the palette out of the way
    slot(v, 0x7f80, 0, 30, 0, 0x20, 0x00);
    slot(v, 0x7f80, 63, 30, 200, 0x1f, 0x00);  // pattern $1F: $FFC0, the last 64 bytes
    for (unsigned c = 0; c < 8; c++) {
        poke(v, (uint16_t)(0x0000 + c), (uint8_t)(0x11 * (c + 1)));
        poke(v, (uint16_t)(0xffc0 + c), (uint8_t)(0xa0 + c));
    }
    row_after(v, 29, row);
    for (unsigned c = 0; c < 8; c++) {
        CHECK_EQ(0x11 * (c + 1), row[32 + c]);
        CHECK_EQ(0xa0 + c, row[32 + 200 + c]);
    }
    free(v);
}

// A restored snapshot has the sprites of the line it was saved building, and
// reports no overflow for them a second time.
TEST(a_restored_card_evaluates_without_reporting) {
    vdp_t *v = graphics();
    vdp_t *w = new_card();
    uint8_t row[VDP_WIDTH], restored[VDP_WIDTH];
    static uint8_t vram[VDP_VRAM_SIZE];
    vdp_snapshot_t s;
    set_reg(v, SPRLIMIT, 1);
    slot(v, 0x0000, 0, 70, 10, 0, 0x07);
    slot(v, 0x0000, 1, 70, 90, 0, 0x07);

    vdp_line_start(v, 70);
    CHECK_EQ(0x41, v->stat0);
    vdp_read(v, 1);                         // acknowledged
    vdp_debug_save(v, &s, vram);
    vdp_build_line(v, row);
    vdp_debug_restore(w, &s, vram);
    CHECK_EQ(0, w->stat0);
    vdp_build_line(w, restored);
    CHECK(memcmp(row, restored, VDP_WIDTH) == 0);
    CHECK(all(row, 32 + 10, 32 + 18, 15));
    CHECK_EQ(0, w->stat0);
    free(v);
    free(w);
}

// PLAN.md section 3: the split is on a 32-pixel boundary within the picture,
// and 0 — nothing for core 0 — on a line with no sprites.
TEST(the_split_is_a_word_boundary_in_the_picture) {
    vdp_t *v = graphics();
    for (unsigned n = 0; n < 32; n++) slot(v, 0x0000, n, 5, (uint8_t)(n * 8), 0, 0x07);
    set_reg(v, SPRLIMIT, 32);
    static const uint8_t vmodes[] = {0x1, 0x2, 0x3, 0x4};
    static const int widths[] = {240, 256, 256, 320};
    for (unsigned m = 0; m < 4; m++) {
        set_reg(v, VMODE, vmodes[m]);
        for (unsigned count = 0; count <= 32; count += 4) {
            set_reg(v, SPRCOUNT, (uint8_t)count);
            for (uint8_t mode1 = DISP; mode1 < DISP + 4; mode1++) {
                set_reg(v, MODE1, mode1);
                vdp_line_start(v, vmodes[m] <= 2 ? 29 : 5);  // display line 6
                int split = vdp_split_choose(v);
                if (!v->sprite_count) {
                    CHECK_EQ(0, split);
                    continue;
                }
                CHECK(split >= widths[m] / 2 - 31 && split <= widths[m]);
                CHECK(split % 32 == 0 || split == widths[m]);
            }
        }
    }
    free(v);
}

int main(void) {
    RUN(every_split_builds_the_same_line);
    RUN(core_0s_collisions_wait_for_the_merge);
    RUN(overflow_at_the_latch_collision_in_the_build);
    RUN(no_sprites_outside_the_picture);
    RUN(sprites_stand_between_the_layers);
    RUN(legacy_and_vmode_read_y_differently);
    RUN(quadrants_flips_and_magnification);
    RUN(stat7_follows_the_last_overflowing_line);
    RUN(the_early_clock_and_the_palette_mapping);
    RUN(a_legacy_colour_0_sprite_collides_unseen);
    RUN(a_write_during_the_build_waits_for_the_next_line);
    RUN(the_sprite_tables_wrap_at_64kb);
    RUN(a_restored_card_evaluates_without_reporting);
    RUN(the_split_is_a_word_boundary_in_the_picture);
    return TEST_RESULT();
}
