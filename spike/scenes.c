// Phase 1 timing spike: the synthetic worst-case scenes. See scenes.h.
//
// Every scene is the worst a line can be at its settings: both layers on with
// per-cell attributes and scroll, layer 1 half transparent, a quarter of the
// cells with the priority bit, random flips; 64 sprite slots all covering the
// band, 32 drawn, every sprite pixel solid, overlapping all the way across.

#include "scenes.h"

#include <stdio.h>
#include <string.h>

// VRAM layout, §7's recommended one with Full mode's 2 KB tables.
enum {
    L0_NAME = 0x0000, L0_ATTR = 0x0800,
    L1_NAME = 0x1000, L1_ATTR = 0x1800,
    SPR_ATTR = 0x2000,
    L0_PAT = 0x4000, L1_PAT = 0x8000, SPR_PAT = 0xC000,
};

static uint32_t rng;
static uint32_t rnd(void) {     // xorshift32
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static void add(spike_scene_t *out, unsigned max, unsigned *n, uint8_t geom, uint8_t depth,
                bool mag, bool detailed, bool tab4, uint8_t limit, bool single) {
    if (*n >= max) return;
    spike_scene_t *s = &out[(*n)++];
    static const char *const depths[] = { "1bpp", "2bpp", "4bpp", "8bpp" };
    char lim[8] = "";
    if (limit != 32) snprintf(lim, sizeof lim, "-lim%u", limit);
    snprintf(s->name, sizeof s->name, "%s-%s-%s%s%s%s%s",
             geom == SPIKE_FULL ? "full" : "graphics", depths[depth], mag ? "32" : "16",
             detailed ? "-det" : "", depth == SPIKE_4BPP && !tab4 ? "-notab" : "", lim,
             single ? "-l0only" : "");
    s->geom = geom; s->depth = depth; s->mag = mag; s->detailed = detailed; s->tab4 = tab4;
    s->limit = limit; s->single = single;
}

unsigned spike_scene_list(spike_scene_t *out, unsigned max) {
    unsigned n = 0;
    for (uint8_t g = 0; g < 2; g++)
        for (uint8_t d = 0; d < 4; d++)
            for (int mag = 0; mag < 2; mag++)
                for (int det = 0; det < 2; det++)
                    add(out, max, &n, g, d, mag, det, true, 32, false);
    // 4bpp by arithmetic instead of the table: Still Open 2.
    for (uint8_t g = 0; g < 2; g++)
        for (int mag = 0; mag < 2; mag++)
            for (int det = 0; det < 2; det++)
                add(out, max, &n, g, SPIKE_4BPP, mag, det, false, 32, false);
    // Still Open 1's remedies, on the worst geometry: a lower SPRLIMIT, and
    // Full mode as a single-layer mode.
    for (int mag = 0; mag < 2; mag++)
        for (int det = 0; det < 2; det++) {
            for (uint8_t lim = 8; lim < 32; lim += 8)
                add(out, max, &n, SPIKE_FULL, SPIKE_4BPP, mag, det, true, lim, false);
            add(out, max, &n, SPIKE_FULL, SPIKE_4BPP, mag, det, true, 32, true);
            add(out, max, &n, SPIKE_FULL, SPIKE_4BPP, mag, det, true, 16, true);
        }
    return n;
}

// A pattern value at `depth`: solid never yields 0; otherwise 0 half the time.
static uint8_t value(uint8_t depth, bool solid) {
    const unsigned bits = 1u << depth, top = (1u << bits) - 1;
    if (!solid && (rnd() & 1)) return 0;
    return (uint8_t)(1 + rnd() % top);
}

static void fill_patterns(spike_t *v, uint32_t base, uint32_t size, uint8_t depth, bool solid) {
    const unsigned bits = 1u << depth;
    for (uint32_t i = 0; i < size; i++) {
        uint8_t b = 0;
        for (unsigned k = 0; k < 8; k += bits) b = (uint8_t)(b << bits | value(depth, solid));
        v->vram[(base + i) & 0xFFFF] = b;
    }
}

void spike_scene_build(spike_t *v, const spike_scene_t *sc) {
    memset(v, 0, sizeof *v);
    rng = 0x6502AC01u;
    spike_set_geometry(v, sc->geom);
    v->use_tab4 = sc->tab4;
    v->backdrop = 0x1F;

    for (unsigned i = 0; i < 256; i++) {
        const uint32_t rgb = (i * 0x9E3u) & 0xFFF;
        v->pal2[i] = rgb | rgb << 16;
    }

    const uint8_t d = sc->depth;
    for (int n = 0; n < 2; n++) {
        spike_layer_t *L = &v->layer[n];
        L->enabled = n == 0 || !sc->single;
        L->opaque = n == 0;
        L->depth = d;
        L->name = n ? L1_NAME : L0_NAME;
        L->attr = n ? L1_ATTR : L0_ATTR;
        L->pat = n ? L1_PAT : L0_PAT;
        L->pal = n ? 5 : 0;
        for (unsigned c = 0; c < 1200; c++) {
            v->vram[L->name + c] = (uint8_t)rnd();
            uint8_t a;
            if (d == SPIKE_1BPP) {
                // fg/bg nibbles; on layer 1 each is 0, transparent, half the time.
                const uint8_t fg = (n && (rnd() & 1)) ? 0 : (uint8_t)(1 + rnd() % 15);
                const uint8_t bg = (n && (rnd() & 1)) ? 0 : (uint8_t)(1 + rnd() % 15);
                a = (uint8_t)(fg << 4 | bg);
            } else {
                a = (uint8_t)(rnd() & 0xBF);                 // sub-palette, flips, bit 8
                if ((rnd() & 3) == 0) a |= 0x40;             // a quarter have priority
            }
            v->vram[L->attr + c] = a;
        }
        fill_patterns(v, L->pat, 0x4000, d, n == 0);
    }
    spike_scene_frame(v, 0);

    v->spr_enabled = true;
    v->collision = true;
    v->detailed = sc->detailed;
    v->d0term = true;
    v->size16 = true;
    v->mag = sc->mag;
    v->spr_depth = d;
    v->sprpal = 3;
    v->sprcount = 64;
    v->sprlimit = sc->limit;
    v->sprattr = SPR_ATTR;
    v->sprpat = SPR_PAT;
    fill_patterns(v, SPR_PAT, 0x4000, d, true);

    const unsigned wpx = 16u << sc->mag;
    for (unsigned s = 0; s < 64; s++) {
        // The first `limit` are drawn, spread as wide as they go with each still
        // overlapping the next; the rest cover the same lines and are dropped.
        unsigned step = (v->width - wpx) / (sc->limit - 1u);
        if (step > wpx - 1) step = wpx - 1;
        const unsigned x = s < sc->limit ? s * step : rnd() % v->width;
        uint8_t a = (uint8_t)(rnd() & 0x3F);                 // sub-palette, flips
        if (rnd() & 1) a |= 0x40;
        if (x & 0x100) a |= 0x80;
        v->vram[SPR_ATTR + 4 * s] = SPIKE_SPRITE_Y;
        v->vram[SPR_ATTR + 4 * s + 1] = (uint8_t)x;
        v->vram[SPR_ATTR + 4 * s + 2] = (uint8_t)(s * 4);
        v->vram[SPR_ATTR + 4 * s + 3] = a;
    }
    spike_vram_sealed(v);
}

void spike_scene_frame(spike_t *v, unsigned n) {
    v->layer[0].scrx = (uint16_t)((379 + 3 * n) & 0x1FF);
    v->layer[0].scry = (uint8_t)(77 + n);
    v->layer[1].scrx = (uint16_t)((5 + 7 * n) & 0x1FF);
    v->layer[1].scry = (uint8_t)(200 + 2 * n);
}
