// §11: the default palette, and the window that feeds the cache.
//
//   test_palette <SPEC.md> <picovdp-default.pal>
//
// The core's table is a transcription (PLAN.md Phase 3). It is checked against
// the table printed in SPEC.md §11, which is normative, and against the JASC
// palette file shipped beside it, so the three cannot drift apart.

#include <stdio.h>
#include <string.h>

#include "card.h"
#include "test.h"
#include "vdp_internal.h"

static const char *spec_path;
static const char *pal_path;

// SPEC.md §11's table: sixteen lines of "   R   rgb rgb ...", rows 0-F.
TEST(default_palette_is_specs_table) {
    FILE *f = fopen(spec_path, "r");
    CHECK(f != NULL);
    if (f == NULL) return;
    char line[512];
    unsigned rows = 0;
    bool in_table = false;
    while (fgets(line, sizeof line, f) && rows < 16) {
        if (!in_table) {
            in_table = strstr(line, "row   0    1    2    3") != NULL;
            continue;
        }
        unsigned row;
        char values[16][4];
        if (sscanf(line, " %1X %3s %3s %3s %3s %3s %3s %3s %3s %3s %3s %3s %3s %3s %3s %3s %3s", &row, values[0],
                   values[1], values[2], values[3], values[4], values[5], values[6], values[7], values[8], values[9],
                   values[10], values[11], values[12], values[13], values[14], values[15]) != 17) {
            continue;  // the rule under the heading
        }
        CHECK_EQ(rows, row);
        for (unsigned i = 0; i < 16; i++) {
            unsigned rgb;
            CHECK(sscanf(values[i], "%3X", &rgb) == 1);
            CHECK_EQ(rgb, vdp_default_palette[16 * row + i]);
        }
        rows++;
    }
    fclose(f);
    CHECK_EQ(16, rows);
    // The ties Python's round broke toward even, which C's round would not (§11).
    CHECK_EQ(0x334, vdp_default_palette[0xf2]);
    CHECK_EQ(0x446, vdp_default_palette[0xf3]);
    CHECK_EQ(0x68a, vdp_default_palette[0xf6]);
}

// picovdp-default.pal: JASC-PAL, 256 lines of 8-bit "r g b", each nibble x 17.
TEST(default_palette_is_the_pal_file) {
    FILE *f = fopen(pal_path, "r");
    CHECK(f != NULL);
    if (f == NULL) return;
    char magic[16];
    unsigned version, count;
    CHECK(fscanf(f, "%15s %u %u", magic, &version, &count) == 3);
    CHECK(strcmp(magic, "JASC-PAL") == 0);
    CHECK_EQ(256, count);
    for (unsigned entry = 0; entry < 256; entry++) {
        unsigned r, g, b;
        CHECK(fscanf(f, "%u %u %u", &r, &g, &b) == 3);
        CHECK(r % 17 == 0 && g % 17 == 0 && b % 17 == 0);
        CHECK_EQ(vdp_default_palette[entry], ((r / 17) << 8) | ((g / 17) << 4) | (b / 17));
    }
    fclose(f);
}

// A power-on card: the table in VRAM at $FC00, two bytes an entry, the cache
// loaded from it, and nothing written at $FE00.
TEST(installed_at_fc00) {
    vdp_t *v = new_card();
    for (unsigned entry = 0; entry < 256; entry++) {
        uint16_t rgb = vdp_default_palette[entry];
        CHECK_EQ((rgb >> 8) & 0xf, vdp_debug_vram(v, (uint16_t)(0xfc00 + 2 * entry)));
        CHECK_EQ(rgb & 0xff, vdp_debug_vram(v, (uint16_t)(0xfc01 + 2 * entry)));
        uint32_t bgr = (uint32_t)((rgb >> 8) & 0xf) | (rgb & 0x0f0u) | ((uint32_t)(rgb & 0xf) << 8);
        CHECK_EQ(bgr * 0x10001u, v->palette[entry]);
        CHECK_EQ(rgb, vdp_debug_palette(v, entry));
    }
    CHECK_EQ(0, vdp_debug_vram(v, 0xfe00));
    CHECK_EQ(0, vdp_debug_vram(v, 0xfbff));
    free(v);
}

// §11: the high nibble of an entry's first byte is ignored, by the cache and
// the debugger alike; and only the window is snooped.
TEST(window_and_ignored_nibble) {
    vdp_t *v = new_card();
    uint8_t bytes[] = {0xa7, 0x3c};
    store(v, 0xfc00 + 2 * 9, bytes, 2);
    uint8_t edge = 0xff;
    store(v, 0xfbff, &edge, 1);
    store(v, 0xfe00, &edge, 1);
    vdp_line_start(v, 0);
    CHECK_EQ(0x73c, vdp_debug_palette(v, 9));
    CHECK_EQ(0x0c37u * 0x10001u, v->palette[9]);
    CHECK_EQ(vdp_default_palette[0], vdp_debug_palette(v, 0));
    CHECK_EQ(vdp_default_palette[255], vdp_debug_palette(v, 255));
    free(v);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: test_palette <SPEC.md> <picovdp-default.pal>\n");
        return 2;
    }
    spec_path = argv[1];
    pal_path = argv[2];
    RUN(default_palette_is_specs_table);
    RUN(default_palette_is_the_pal_file);
    RUN(installed_at_fc00);
    RUN(window_and_ignored_nibble);
    return TEST_RESULT();
}
