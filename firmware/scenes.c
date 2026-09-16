// Phase 1's worst-case scenes as port writes. See scenes.h.

#include "scenes.h"

#include <stdio.h>
#include <string.h>

// §5's registers the scenes write.
enum {
    MODE1 = 0x01, COLOR = 0x07, VBANK = 0x08, VINC = 0x09, IRQEN = 0x0a, PALBASE = 0x0c, VMODE = 0x0d,
    STATSEL_A = 0x0f,
    L0NAME = 0x10, L1NAME = 0x18,  // each layer's block: NAME ATTR PAT SCRX SCRY CTRL PAL
    SPRATTR = 0x20, SPRPAT = 0x21, SPRCOUNT = 0x22, SPRCTRL = 0x23, SPRLIMIT = 0x24, SPRPAL = 0x25,
    FONT = 0x30,
};
enum { NAME, ATTR, PAT, SCRX, SCRY, CTRL, PAL };

// Phase 1's VRAM layout, with the palette moved from $FC00, which the sprite
// patterns reach, to $3C00.
enum {
    L0_NAME = 0x0000, L0_ATTR = 0x0800, L1_NAME = 0x1000, L1_ATTR = 0x1800,
    SPR_ATTR = 0x2000, PALETTE = 0x3c00,
    L0_PAT = 0x4000, L1_PAT = 0x8000, SPR_PAT = 0xc000,
    FONT_0 = 0x2800, FONT_1 = 0x3000,  // unused: where scene_fonts's loads land
};

#define SPRITE_Y 100
#define CELLS 1200

static scene_t scenes[40];
static unsigned count;

static void add(uint8_t vmode, uint8_t depth, bool magnified, bool detailed, uint8_t limit) {
    static const char *const depths[] = {"1bpp", "2bpp", "4bpp", "8bpp"};
    scene_t *s = &scenes[count++];
    char lim[8] = "";
    if (limit != 32) snprintf(lim, sizeof lim, "-lim%u", limit);
    snprintf(s->name, sizeof s->name, "%s-%s-%s%s%s", vmode == 4 ? "full" : "graphics", depths[depth],
             magnified ? "32" : "16", detailed ? "-det" : "", lim);
    s->vmode = vmode;
    s->depth = depth;
    s->magnified = magnified;
    s->detailed = detailed;
    s->limit = limit;
}

static void list(void) {
    if (count) return;
    for (uint8_t vmode = 3; vmode <= 4; vmode++)
        for (uint8_t depth = 0; depth < 4; depth++)
            for (int magnified = 0; magnified < 2; magnified++)
                for (int detailed = 0; detailed < 2; detailed++) add(vmode, depth, magnified, detailed, 32);
    for (int magnified = 0; magnified < 2; magnified++)
        for (int detailed = 0; detailed < 2; detailed++) add(4, 2, magnified, detailed, 16);
}

unsigned scene_count(void) {
    list();
    return count;
}

const scene_t *scene_at(unsigned index) {
    list();
    return index < count ? &scenes[index] : NULL;
}

int scene_find(const char *name) {
    list();
    for (unsigned i = 0; i < count; i++) {
        if (!strcmp(scenes[i].name, name)) return (int)i;
    }
    return -1;
}

// ---- the program ----

typedef struct writer {
    const scene_port_t *port;
    uint32_t rng;
} writer_t;

static uint32_t rnd(writer_t *w) {  // xorshift32, as Phase 1's
    w->rng ^= w->rng << 13;
    w->rng ^= w->rng >> 17;
    w->rng ^= w->rng << 5;
    return w->rng;
}

static void reg(const scene_port_t *port, unsigned index, uint8_t value) {
    port->write(port->context, 1, value);
    port->write(port->context, 1, (uint8_t)(0x80 | index));
}

// §4: VBANK for the top bits, then a write address on port A.
static void address(const scene_port_t *port, uint16_t at) {
    reg(port, VBANK, (uint8_t)(at >> 14));
    port->write(port->context, 1, (uint8_t)at);
    port->write(port->context, 1, (uint8_t)(0x40 | ((at >> 8) & 0x3f)));
}

static void data(const scene_port_t *port, uint8_t value) {
    port->write(port->context, 0, value);
}

// A pattern value at depth: solid never yields 0; otherwise 0 half the time.
static uint8_t value(writer_t *w, unsigned depth, bool solid) {
    unsigned bits = 1u << depth, top = (1u << bits) - 1;
    if (!solid && (rnd(w) & 1)) return 0;
    return (uint8_t)(1 + rnd(w) % top);
}

static void patterns(writer_t *w, uint16_t at, unsigned size, unsigned depth, bool solid) {
    unsigned bits = 1u << depth;
    address(w->port, at);
    for (unsigned i = 0; i < size; i++) {
        uint8_t b = 0;
        for (unsigned k = 0; k < 8; k += bits) b = (uint8_t)(b << bits | value(w, depth, solid));
        data(w->port, b);
    }
}

void scene_setup(const scene_t *s, const scene_port_t *port) {
    writer_t w = {port, 0x6502ac01u};
    unsigned width = s->vmode == 4 ? 320 : 256;

    reg(port, VINC, 0x01);
    reg(port, VMODE, s->vmode);
    reg(port, COLOR, 0x0f);
    reg(port, IRQEN, 0x0c);  // overflow and collision, for STAT1

    // A palette that is not black: Phase 1's, at $3C00.
    reg(port, PALBASE, PALETTE >> 10);
    address(port, PALETTE);
    for (unsigned i = 0; i < 256; i++) {
        unsigned rgb = (i * 0x9e3u) & 0xfff;
        data(port, (uint8_t)(rgb >> 8));
        data(port, (uint8_t)rgb);
    }

    for (unsigned n = 0; n < 2; n++) {
        const unsigned block = n ? L1NAME : L0NAME;
        const uint16_t name = n ? L1_NAME : L0_NAME, attr = n ? L1_ATTR : L0_ATTR, pat = n ? L1_PAT : L0_PAT;
        reg(port, block + NAME, (uint8_t)(name >> 10));
        reg(port, block + ATTR, (uint8_t)(attr >> 10));
        reg(port, block + PAT, (uint8_t)(pat >> 11));
        reg(port, block + PAL, n ? 5 : 0);

        // Names, then attributes: per cell (§8), in the order Phase 1 drew them.
        uint8_t names[CELLS], attributes[CELLS];
        for (unsigned c = 0; c < CELLS; c++) {
            names[c] = (uint8_t)rnd(&w);
            uint8_t a;
            if (s->depth == 0) {
                uint8_t fg = (n && (rnd(&w) & 1)) ? 0 : (uint8_t)(1 + rnd(&w) % 15);
                uint8_t bg = (n && (rnd(&w) & 1)) ? 0 : (uint8_t)(1 + rnd(&w) % 15);
                a = (uint8_t)(fg << 4 | bg);
            } else {
                a = (uint8_t)(rnd(&w) & 0xbf);          // sub-palette, flips, bit 8
                if ((rnd(&w) & 3) == 0) a |= 0x40;      // a quarter have priority
            }
            attributes[c] = a;
        }
        address(port, name);
        for (unsigned c = 0; c < CELLS; c++) data(port, names[c]);
        address(port, attr);
        for (unsigned c = 0; c < CELLS; c++) data(port, attributes[c]);
        patterns(&w, pat, 0x4000, s->depth, n == 0);
    }

    // Sprites: 64 slots covering the band, the first SPRLIMIT drawn.
    reg(port, SPRATTR, (uint8_t)(SPR_ATTR >> 7));
    reg(port, SPRPAT, (uint8_t)(SPR_PAT >> 11));
    reg(port, SPRCOUNT, 64);
    reg(port, SPRLIMIT, s->limit);
    reg(port, SPRPAL, 3);
    patterns(&w, SPR_PAT, 0x4000, s->depth, true);
    const unsigned span = 16u << s->magnified;
    address(port, SPR_ATTR);
    for (unsigned n = 0; n < 64; n++) {
        unsigned step = (width - span) / (s->limit - 1u);
        if (step > span - 1) step = span - 1;
        unsigned x = n < s->limit ? n * step : rnd(&w) % width;
        uint8_t a = (uint8_t)(rnd(&w) & 0x3f);  // sub-palette, flips
        if (rnd(&w) & 1) a |= 0x40;
        if (x & 0x100) a |= 0x80;
        data(port, SPRITE_Y);
        data(port, (uint8_t)x);
        data(port, (uint8_t)(n * 4));
        data(port, a);
    }
    // Enabled, collision, the $D0 terminator, detailed, the depth.
    reg(port, SPRCTRL, (uint8_t)(0x07 | (s->detailed ? 0x08 : 0) | (s->depth << 4)));

    scene_frame(s, 0, port);
    // Display on, 16 x 16 sprites, magnified or not.
    reg(port, MODE1, (uint8_t)(0x42 | (s->magnified ? 0x01 : 0)));
}

void scene_frame(const scene_t *s, unsigned n, const scene_port_t *port) {
    static const uint16_t x0 = 379, x1 = 5;
    const unsigned scroll[2][2] = {
        {(x0 + 3 * n) & 0x1ff, (77 + n) & 0xff},
        {(x1 + 7 * n) & 0x1ff, (200 + 2 * n) & 0xff},
    };
    for (unsigned layer = 0; layer < 2; layer++) {
        const unsigned block = layer ? L1NAME : L0NAME;
        // Depth, per-cell attributes, enabled, layer 0 opaque, and scroll bit 8.
        uint8_t control = (uint8_t)(s->depth | 0x10 | (layer ? 0 : 0x20) | ((scroll[layer][0] & 0x100) ? 0x40 : 0));
        reg(port, block + SCRX, (uint8_t)scroll[layer][0]);
        reg(port, block + SCRY, (uint8_t)scroll[layer][1]);
        reg(port, block + CTRL, control);
    }
}

void scene_fonts(const scene_t *s, const scene_port_t *port) {
    (void)s;
    for (unsigned layer = 0; layer < 2; layer++) {
        const unsigned block = layer ? L1NAME : L0NAME;
        reg(port, block + PAT, (uint8_t)((layer ? FONT_1 : FONT_0) >> 11));
        reg(port, FONT, layer ? 0x80 : 0x00);
        reg(port, block + PAT, (uint8_t)((layer ? L1_PAT : L0_PAT) >> 11));
    }
}

void scene_reads(const scene_port_t *port, uint8_t reads[SCENE_READS]) {
    static const uint8_t order[SCENE_READS] = {1, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0};
    for (unsigned i = 0; i < SCENE_READS; i++) {
        reg(port, STATSEL_A, order[i]);
        reads[i] = port->read(port->context, 1);
    }
}
