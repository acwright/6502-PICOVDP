// Phase 1 timing spike: the synthetic worst-case scenes (PLAN.md Phase 1).

#pragma once

#include "render.h"

typedef struct {
    char name[32];
    uint8_t geom;       // SPIKE_GRAPHICS or SPIKE_FULL
    uint8_t depth;      // both layers and the sprites
    bool mag;           // 16 × 16 sprites magnified to 32 × 32
    bool detailed;      // SPRCTRL b3
    bool tab4;          // 4bpp unpacking table
    uint8_t limit;      // SPRLIMIT: this many sprites drawn on the band
    bool single;        // layer 1 off: Still Open 1's third remedy
} spike_scene_t;

// Every scene the spike times, in report order. Returns the count.
unsigned spike_scene_list(spike_scene_t *out, unsigned max);

// Fills VRAM and registers for a scene. Deterministic.
void spike_scene_build(spike_t *v, const spike_scene_t *sc);

// Moves both layers' scroll for frame n, so every cell offset is exercised.
void spike_scene_frame(spike_t *v, unsigned n);

// The display lines the sprites cover: the worst case lives here.
#define SPIKE_SPRITE_Y 100
static inline bool spike_in_sprite_band(const spike_t *v, int line) {
    return line >= SPIKE_SPRITE_Y && line < SPIKE_SPRITE_Y + (16 << v->mag);
}
