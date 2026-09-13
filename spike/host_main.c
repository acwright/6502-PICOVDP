// Phase 1 timing spike, host side: proves the renderer being timed draws
// what SPEC.md says, by comparing every scene, built both on one core and
// split across two, against a per-pixel reference
// written straight from §8, §10, §12 and §13, and writes each scene's first
// frame as a PPM for a look. Usage: spike_host [out-dir]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "render.h"
#include "scenes.h"

static spike_t v, ref_v;
static spike_line_t ln;
static spike_sprline_t sprline;

static uint32_t rng = 1;
static uint32_t rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

// Beyond the worst cases: every register the renderer reads, at random, and
// sprites anywhere — off each edge, entering from the top and left, 8 × 8.
static void randomise(spike_t *s, uint32_t seed) {
    rng = seed * 2654435761u + 1;
    for (int k = 0; k < 2; k++) {
        s->layer[k].enabled = rnd() % 4 != 0;
        s->layer[k].opaque = rnd() & 1;
        s->layer[k].pal = rnd() & 15;
    }
    s->spr_enabled = rnd() % 8 != 0;
    s->collision = rnd() % 4 != 0;
    s->detailed = rnd() & 1;
    s->d0term = rnd() & 1;
    s->size16 = rnd() & 1;
    s->mag = rnd() & 1;
    s->sprpal = rnd() & 15;
    s->sprcount = rnd() % 65;
    s->sprlimit = rnd() % 33;
    s->backdrop = (uint8_t)rnd();
    for (unsigned n = 0; n < 64; n++) {
        s->vram[s->sprattr + 4 * n] = (uint8_t)(rnd() % 8 == 0 ? 0xD0 : rnd());
        s->vram[s->sprattr + 4 * n + 1] = (uint8_t)rnd();
        s->vram[s->sprattr + 4 * n + 2] = (uint8_t)rnd();
        s->vram[s->sprattr + 4 * n + 3] = (uint8_t)rnd();
    }
    // A quarter of the scenes: a few sprites crowding the right edge, so a
    // collision off the picture — which must not count, §10 — is not hidden
    // behind one on it.
    if (rnd() % 4 == 0) {
        s->sprcount = 2 + rnd() % 3;
        s->d0term = false;
        for (unsigned n = 0; n < s->sprcount; n++) {
            const unsigned x = s->width - 12 + rnd() % 20;
            s->vram[s->sprattr + 4 * n] = (uint8_t)(rnd() % 200);
            s->vram[s->sprattr + 4 * n + 1] = (uint8_t)x;
            s->vram[s->sprattr + 4 * n + 3] = (uint8_t)((rnd() & 0x7F) | (x & 0x100 ? 0x80 : 0));
        }
    }
    // Sparse sprite patterns, so transparency inside a sprite is exercised.
    for (unsigned i = 0; i < 0x4000; i++)
        if (rnd() % 3 == 0) s->vram[s->sprpat + i] = 0;
    spike_vram_sealed(s);
}

// The value of pixel (c, r) of an 8 × 8 pattern at `base`, packed MSB first, §8.
static unsigned pattern_value(const spike_t *s, uint32_t base, uint8_t depth, unsigned r, unsigned c) {
    const unsigned bpp = 1u << depth;
    const uint32_t bit = c * bpp;
    const uint8_t byte = s->vram[(base + r * (bpp) + bit / 8) & 0xFFFF];
    return (byte >> (8 - bpp - bit % 8)) & ((1u << bpp) - 1);
}

typedef struct { uint8_t idx, level; bool solid; } cand_t;

static cand_t ref_layer(const spike_t *s, const spike_layer_t *L, int line, int px, uint8_t lvlbase) {
    cand_t out = { 0, 0, false };
    const unsigned W = s->width;
    const unsigned mapx = (px + L->scrx % W) % W;                  // §13
    const unsigned mapy = (line + L->scry % 240u) % 240u;
    const unsigned cell = (mapy / 8) * s->cols + mapx / 8;
    const uint8_t name = s->vram[(L->name + cell) & 0xFFFF];
    const uint8_t a = s->vram[(L->attr + cell) & 0xFFFF];
    unsigned c = mapx % 8, r = mapy % 8;
    const uint8_t d = L->depth;
    const unsigned bpp = 1u << d;

    if (d == SPIKE_1BPP) {
        const unsigned bit = pattern_value(s, L->pat + name * 8u, d, r, c);
        const unsigned nib = bit ? a >> 4 : (a & 15u);
        out.idx = (uint8_t)(L->pal * 16u + nib);
        out.solid = nib != 0 || L->opaque;
        out.level = lvlbase;
        return out;
    }
    const unsigned tile = d == SPIKE_8BPP ? name : (name | (a & 0x80u) << 1);
    if (a & 0x10) c = 7 - c;
    if (a & 0x20) r = 7 - r;
    const unsigned val = pattern_value(s, L->pat + tile * (8u << d), d, r, c);
    out.idx = (uint8_t)(((L->pal * 16u + (a & 15u)) << bpp) + val);
    out.solid = val != 0 || L->opaque;
    out.level = (a & 0x40) ? lvlbase + 3 : lvlbase;
    return out;
}

static void ref_frame(spike_t *s, uint8_t *frame) {
    const unsigned src_size = s->size16 ? 16u : 8u, size = src_size << s->mag;
    for (int line = 0; line < 240; line++) {
        // Evaluation, §10
        uint8_t list[32];
        unsigned n = 0;
        bool dropped = false;
        for (unsigned i = 0; s->spr_enabled && i < s->sprcount; i++) {
            const uint8_t y = s->vram[s->sprattr + 4 * i];
            if (s->d0term && y == 0xD0) break;
            const int top = y > 240 ? y - 256 : y;
            if (line < top || line >= top + (int)size) continue;
            if (n < s->sprlimit) list[n++] = (uint8_t)i;
            else if (!dropped) {
                dropped = true;
                if (!s->ovf) { s->ovf = true; s->first_drop = (uint8_t)i; }
            }
        }

        for (int x = 0; x < 320; x++) {
            uint8_t *out = &frame[line * 320 + x];
            const int px = x - s->left;
            if (px < 0 || px >= s->width) { *out = s->backdrop; continue; }

            cand_t best = { s->backdrop, 0, true };
            for (int k = 0; k < 2; k++) {
                if (!s->layer[k].enabled) continue;
                const cand_t c = ref_layer(s, &s->layer[k], line, px, k ? 3 : 1);
                if (c.solid && c.level > best.level) best = c;
            }

            // Sprites: the lowest index with a solid pixel owns it; collision first.
            int owner = -1;
            cand_t spr = { 0, 0, false };
            for (unsigned i = 0; i < n; i++) {
                const unsigned sn = list[i];
                const uint8_t *e = &s->vram[s->sprattr + 4 * sn];
                const int top = e[0] > 240 ? e[0] - 256 : e[0];
                const unsigned x9 = e[1] | (e[3] & 0x80u) << 1;
                const int sx = x9 >= 384 ? (int)x9 - 512 : (int)x9;
                if (px < sx || px >= sx + (int)size) continue;
                unsigned r = (unsigned)(line - top) >> s->mag, c = (unsigned)(px - sx) >> s->mag;
                if (e[3] & 0x10) c = src_size - 1 - c;
                if (e[3] & 0x20) r = src_size - 1 - r;
                const unsigned q = e[2] + (c >= 8 ? 2 : 0) + (r >= 8 ? 1 : 0);   // quadrants, 16 × 16 only
                const unsigned val = pattern_value(s, s->sprpat + q * (8u << s->spr_depth), s->spr_depth, r % 8, c % 8);
                if (!val) continue;
                if (owner < 0) {
                    owner = (int)sn;
                    spr.idx = (uint8_t)(((s->sprpal * 16u + (e[3] & 15u)) << (1u << s->spr_depth)) + val);
                    spr.level = (e[3] & 0x40) ? 5 : 2;
                    spr.solid = true;
                } else if (s->collision) {
                    s->col = true;
                    if (s->detailed) s->colmap |= 1ull << sn | 1ull << owner;
                }
            }
            if (spr.solid && spr.level > best.level) best = spr;
            *out = best.idx;
        }
    }
}

static void write_ppm(const char *path, const spike_t *s, const uint8_t *frame) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n320 240\n255\n");
    for (unsigned i = 0; i < 320 * 240; i++) {
        const uint32_t rgb = s->pal2[frame[i]] & 0xFFF;    // 0x0BGR
        const uint8_t px[3] = { (uint8_t)((rgb & 15) * 17), (uint8_t)((rgb >> 4 & 15) * 17),
                                (uint8_t)((rgb >> 8 & 15) * 17) };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv) {
    const char *outdir = argc > 1 ? argv[1] : NULL;
    static spike_scene_t scenes[80];
    static uint8_t got[320 * 240], want[320 * 240];
    static uint16_t rgb[640];
    const unsigned nscenes = spike_scene_list(scenes, 80);
    const unsigned nrandom = 400;
    unsigned failures = 0;

    spike_init_tables();
    for (unsigned i = 0; i < nscenes + nrandom; i++) {
        const spike_scene_t *sc = &scenes[i < nscenes ? i : i % nscenes];
        spike_scene_build(&v, sc);
        spike_scene_build(&ref_v, sc);
        const bool random = i >= nscenes;
        if (random) {
            randomise(&v, i);
            randomise(&ref_v, i);
        }
        for (unsigned frame = 0; frame < (random ? 2u : 9u); frame++) {
            spike_scene_frame(&v, frame);
            spike_scene_frame(&ref_v, frame);
            ref_v.ovf = ref_v.col = false; ref_v.colmap = 0; ref_v.first_drop = 0;
            ref_frame(&ref_v, want);

            // Three ways: sprites straight into the line; all on core 0 and merged;
            // and divided at the middle of the picture, left half on core 0.
            for (int split = 0; split < 3; split++) {
            const char *const how = split == 2 ? " (split at half)" : split ? " (split)" : "";
            const int xs = split == 2 ? v.width / 2 : v.width;
            v.ovf = v.col = false; v.colmap = 0; v.first_drop = 0;

            for (int line = 0; line < 240; line++) {
                if (split) spike_build_line_split(&v, &ln, &sprline, line, xs, rgb);
                else spike_build_line(&v, &ln, line, rgb);
                memcpy(got + line * 320, SPIKE_IDX(&ln), 320);
                for (unsigned x = 0; x < 320; x++) {
                    uint32_t want_rgb = v.pal2[got[line * 320 + x]];
                    if (memcmp(rgb + 2 * x, &want_rgb, 4) != 0) {
                        fprintf(stderr, "%s%s: expansion wrong at line %d x %u\n", sc->name, how, line, x);
                        failures++;
                        break;
                    }
                }
            }

            unsigned bad = 0, first = 0;
            for (unsigned p = 0; p < sizeof got; p++)
                if (got[p] != want[p] && bad++ == 0) first = p;
            if (bad) {
                fprintf(stderr, "%s%s frame %u: %u pixels differ, first at line %u x %u (got %02X want %02X)\n",
                        sc->name, how, frame, bad, first / 320, first % 320, got[first], want[first]);
                if (getenv("SPIKE_DEBUG")) {
                    fprintf(stderr, "  random %d L0 en %d op %d L1 en %d op %d spr en %d col %d det %d d0 %d 16 %d mag %d count %u limit %u left %u\n",
                            random, v.layer[0].enabled, v.layer[0].opaque, v.layer[1].enabled, v.layer[1].opaque,
                            v.spr_enabled, v.collision, v.detailed, v.d0term, v.size16, v.mag, v.sprcount, v.sprlimit, v.left);
                    const int line = (int)(first / 320), px = (int)(first % 320) - v.left;
                    for (unsigned n = 0; n < v.sprcount; n++) {
                        const uint8_t *e = &v.vram[v.sprattr + 4 * n];
                        const int top = e[0] > 240 ? e[0] - 256 : e[0];
                        const unsigned x9 = e[1] | (e[3] & 0x80u) << 1;
                        const int sx = x9 >= 384 ? (int)x9 - 512 : (int)x9;
                        const int sz = (v.size16 ? 16 : 8) << v.mag;
                        if (line >= top && line < top + sz && px >= sx && px < sx + sz)
                            fprintf(stderr, "  sprite %u y %d x %d pat %u attr %02X\n", n, top, sx, e[2], e[3]);
                    }
                }
                failures++;
            }
            if (v.ovf != ref_v.ovf || v.first_drop != ref_v.first_drop || v.col != ref_v.col ||
                v.colmap != ref_v.colmap) {
                fprintf(stderr, "%s%s frame %u: status differs: ovf %d/%d drop %u/%u col %d/%d map %016llX/%016llX\n",
                        sc->name, how, frame, v.ovf, ref_v.ovf, v.first_drop, ref_v.first_drop, v.col, ref_v.col,
                        (unsigned long long)v.colmap, (unsigned long long)ref_v.colmap);
                failures++;
            }
            }
            if (outdir && frame == 0 && !random) {
                char path[512];
                snprintf(path, sizeof path, "%s/%s.ppm", outdir, sc->name);
                write_ppm(path, &v, got);   // the split path's frame, which matched
            }
        }
        if (!random && (!ref_v.ovf || !ref_v.col || (sc->detailed && ref_v.colmap == 0))) {
            fprintf(stderr, "%s: scene is not a worst case (ovf %d col %d)\n", sc->name, ref_v.ovf, ref_v.col);
            failures++;
        }
    }
    printf("%u worst-case scenes of 9 frames and %u random ones of 2: %s\n", nscenes, nrandom,
           failures ? "FAILED" : "renderer matches the reference");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
