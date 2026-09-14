// Phase 8's word-at-a-time renderer against Phase 7's pixel-at-a-time one
// (reference.c), which Video.test.ts, the goldens and 10^5 fuzzed frames held
// to Video.ts.
//
// Random cards — every geometry and legacy mode, every depth, attribute source,
// flip, priority bit, scroll and palette group, layers on and off, index 0
// opaque or not, sprites of every size, depth and flag, crowded onto the line,
// off both edges, wrapping VRAM at 64 KB — and many lines of each, built in two
// halves at several splits between the cores. Every row must match the
// reference's, and so must the collision it publishes.

#include <string.h>

#include "card.h"
#include "reference.h"
#include "test.h"

static uint32_t rng_state = 1;
static uint32_t rng(void) {  // xorshift32
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
static unsigned below(unsigned n) {
    return rng() % n;
}

// A register, straight into the file: these tests are about the render side.
static void set(vdp_t *v, unsigned index, uint8_t value) {
    vdp_debug_set_register(v, index, value);
}

static void scene(vdp_t *v, unsigned *focus) {
    vdp_reset(v, true);
    // VRAM: runs of zeros, of $FF, and noise, so layers and sprites are solid,
    // clear and ragged.
    for (unsigned a = 0; a < VDP_VRAM_SIZE;) {
        unsigned run = 1 + below(64);
        unsigned kind = below(4);
        for (unsigned i = 0; i < run && a < VDP_VRAM_SIZE; i++, a++) {
            vdp_debug_set_vram(v, (uint16_t)a, kind == 0 ? 0x00 : kind == 1 ? 0xff : (uint8_t)rng());
        }
    }

    // The mode: VMODE $1-$4, or the legacy submode with M1, M2 and M3.
    unsigned mode = below(6);
    uint8_t mode1 = (uint8_t)((below(10) ? 0x40 : 0) | (rng() & 0x03));
    if (mode < 4) {
        set(v, 0x0d, (uint8_t)(mode + 1));
    } else {
        set(v, 0x0d, 0x00);
        mode1 |= (uint8_t)(rng() & 0x18);
        set(v, 0x00, (uint8_t)(rng() & 0x02));
    }
    set(v, 0x01, mode1);
    set(v, 0x07, (uint8_t)rng());

    for (unsigned layer = 0; layer < 2; layer++) {
        unsigned block = layer ? 0x18 : 0x10;
        // Tables anywhere, the top of VRAM included, so reads wrap.
        for (unsigned r = 0; r < 3; r++) set(v, block + r, (uint8_t)rng());
        set(v, block + 3, (uint8_t)rng());
        set(v, block + 4, (uint8_t)rng());
        uint8_t control = (uint8_t)(rng() & 0x6f);
        if (below(5)) control |= 0x10;
        set(v, block + 5, control);
        set(v, block + 6, (uint8_t)rng());
    }

    // Sprites crowded onto the focus line, some off either edge.
    set(v, 0x20, (uint8_t)rng());
    set(v, 0x21, (uint8_t)rng());
    set(v, 0x22, (uint8_t)below(72));
    set(v, 0x23, (uint8_t)((rng() & 0x3e) | (below(8) ? 0x01 : 0)));
    set(v, 0x24, (uint8_t)below(40));
    set(v, 0x25, (uint8_t)rng());
    *focus = below(240);
    uint16_t table = (uint16_t)(vdp_debug_register(v, 0x20) << 7);
    for (unsigned n = 0; n < 64; n++) {
        uint16_t at = (uint16_t)(table + 4 * n);
        int y = (int)*focus - (int)below(34);
        vdp_debug_set_vram(v, at, below(16) ? (uint8_t)(y & 0xff) : (uint8_t)rng());
        uint8_t x = below(4) ? (uint8_t)below(256) : (uint8_t)rng();
        vdp_debug_set_vram(v, (uint16_t)(at + 1), x);
        vdp_debug_set_vram(v, (uint16_t)(at + 2), (uint8_t)rng());
        vdp_debug_set_vram(v, (uint16_t)(at + 3), (uint8_t)rng());
    }
    vdp_line_start(v, 0);
}

TEST(words_draw_what_pixels_did) {
    static vdp_t card, copy;
    vdp_init(&card, 0x04);
    uint8_t reference[VDP_WIDTH], row[VDP_WIDTH];
    unsigned lines = 0, pictured = 0, with_sprites = 0, collided = 0, mapped = 0, text = 0, legacy = 0, differ = 0;
    static const int splits[] = {0, 32, 96, 160, 256, 320, 7, 101, 255, -1};  // -1: vdp_build_line's own

    for (unsigned n = 0; n < 1500 && differ < 5; n++) {
        unsigned focus;
        scene(&card, &focus);
        vdp_debug_mode_t mode = vdp_debug_mode(&card);
        for (int dy = -40; dy <= 40; dy += 5) {
            uint16_t screen = (uint16_t)((focus + mode.origin_y + VDP_SCREEN_LINES + (unsigned)dy) % VDP_SCREEN_LINES);
            uint16_t latch = (uint16_t)((screen + VDP_SCREEN_LINES - 1) % VDP_SCREEN_LINES);
            vdp_line_start(&card, latch);
            card.stat0 = 0;
            card.frame_events = 0;
            memset(card.collision_map, 0, sizeof card.collision_map);

            bool ref_collided;
            uint64_t ref_collisions;
            reference_build_line(&card, reference, &ref_collided, &ref_collisions);
            lines++;
            if (card.render_screen_line < VDP_HEIGHT && (card.render_reg[0x01] & 0x40)) pictured++;
            if (card.sprite_count) with_sprites++;
            if (ref_collided) collided++;
            if (ref_collisions) mapped++;
            if (mode.cell_width == 6) text++;
            if (mode.legacy) legacy++;

            for (unsigned s = 0; s < sizeof splits / sizeof splits[0]; s++) {
                memcpy(&copy, &card, sizeof copy);
                static vdp_half_t core0;
                if (splits[s] < 0) {
                    vdp_build_line(&copy, row);
                } else {
                    // Stale, as the firmware's halves are from the line before.
                    memset(core0.line, 0xa5, sizeof core0.line);
                    memset(core0.level, 5, sizeof core0.level);
                    memset(copy.half.line, 0x5a, sizeof copy.half.line);
                    vdp_build_half(&copy, &core0, 0, splits[s]);
                    vdp_build_half(&copy, &copy.half, splits[s], VDP_WIDTH);
                    vdp_publish(&copy, &core0, &copy.half);
                    memset(row, 0xee, sizeof row);
                    vdp_copy_half(&copy, &core0, row);
                    vdp_copy_half(&copy, &copy.half, row);
                    // And the halves' expansions make the whole row's.
                    uint16_t whole[2 * VDP_WIDTH], halves[2 * VDP_WIDTH];
                    vdp_expand_line(&copy, row, whole);
                    memset(halves, 0xee, sizeof halves);
                    vdp_expand_half(&copy, &core0, halves);
                    vdp_expand_half(&copy, &copy.half, halves);
                    CHECK(memcmp(whole, halves, sizeof whole) == 0);
                }
                uint64_t map = 0;
                for (unsigned b = 0; b < 8; b++) map |= (uint64_t)copy.collision_map[b] << (8 * b);
                bool same = memcmp(row, reference, VDP_WIDTH) == 0 && ((copy.stat0 & 0x20) != 0) == ref_collided &&
                            map == ref_collisions;
                CHECK(same);
                if (!same && differ++ < 5) {
                    unsigned x = 0;
                    while (x < VDP_WIDTH && row[x] == reference[x]) x++;
                    printf("  scene %u, screen line %u, split %d: vmode %u legacy %u, sprites %u; first column %u: %u, the "
                           "reference %u; COL %d/%d, map %llx/%llx\n",
                           n, card.render_screen_line, splits[s], mode.vmode, mode.legacy, card.sprite_count, x,
                           x < VDP_WIDTH ? row[x] : 0, x < VDP_WIDTH ? reference[x] : 0, (copy.stat0 & 0x20) != 0,
                           ref_collided, (unsigned long long)map, (unsigned long long)ref_collisions);
                }
            }
        }
    }
    printf("  %u lines: %u in a picture, %u with sprites, %u colliding, %u with a detailed map, %u in Text, %u legacy\n",
           lines, pictured, with_sprites, collided, mapped, text, legacy);
    CHECK(pictured > lines / 2);
    CHECK(with_sprites > lines / 5);
    CHECK(collided > lines / 20);
    CHECK(mapped > lines / 50);
    CHECK(text > lines / 10);
    CHECK(legacy > lines / 5);
}

int main(void) {
    RUN(words_draw_what_pixels_did);
    return TEST_RESULT();
}
