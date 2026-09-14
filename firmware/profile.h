// PROFILE: one row's stages timed on core 1 (profile.c, docs/DEBUGLINK.md).

#pragma once

#include <stdint.h>

#include "vdp.h"

enum {
    PROFILE_EVALUATE,        // §10's evaluation of the row's sprites
    PROFILE_LAYER0,          // layer 0 across the picture, levels kept
    PROFILE_LAYER1,          // layer 1 over it, judged
    PROFILE_ROW,             // the whole row as one half: backdrop, both layers, the sprites
    PROFILE_LEFT,            // the left half at the picture's middle
    PROFILE_RIGHT,           // the right half
    PROFILE_EXPAND,          // the whole row's 320 indices to 640 pixels
    PROFILE_SPLIT_CHOOSE,
    PROFILE_STAGES,
};

typedef struct profile {
    uint16_t row;
    uint8_t sprites;
    uint32_t probe;          // two reads of the counter
    uint32_t min[PROFILE_STAGES];
    uint32_t max[PROFILE_STAGES];
} profile_t;

// On core 1's thread, between lines.
void profile_row(vdp_t *v, uint16_t row, uint32_t iterations, profile_t *out);
