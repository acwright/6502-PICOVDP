// Phase 7's pixel-at-a-time renderer, the reference for test_render. See reference.c.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "vdp.h"

// The row the render side has caught up to, into 320 indices, and the
// collision the sprites on it found. Status is not published.
void reference_build_line(vdp_t *v, uint8_t *indices, bool *collided, uint64_t *collisions);
