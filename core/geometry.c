// §9: the four geometries, and which one a register file selects.
//
// Needed from Phase 3 for the debugger's view of the name table; the renderer
// and the display line (§3) take it up in Phases 4 and 5.

#include "vdp_internal.h"

// x and y centre the picture in the 320 x 240 frame (§3's position column).
static const vdp_geometry_t geometries[] = {
    [VDP_GEOMETRY_TEXT] = {VDP_GEOMETRY_TEXT, 40, 24, 6, 240, 192, 40, 24},
    [VDP_GEOMETRY_COMPACT] = {VDP_GEOMETRY_COMPACT, 32, 24, 8, 256, 192, 32, 24},
    [VDP_GEOMETRY_GRAPHICS] = {VDP_GEOMETRY_GRAPHICS, 32, 30, 8, 256, 240, 32, 0},
    [VDP_GEOMETRY_FULL] = {VDP_GEOMETRY_FULL, 40, 30, 8, 320, 240, 0, 0},
};

const vdp_geometry_t *vdp_geometry(const uint8_t *reg, vdp_legacy_mode_t *legacy) {
    // VMODE $1-$4 name a geometry; $0 and the reserved $5-$F hand the choice to
    // M1, M2 and M3 (§9).
    unsigned vmode = reg[VDP_REG_VMODE] & 0x0f;
    if (vmode >= 1 && vmode <= 4) {
        if (legacy) *legacy = VDP_LEGACY_NONE;
        return &geometries[vmode - 1];
    }
    // §9's table, row by row: M1 is "1 × ×", so Text beats both others; M2 is
    // "0 1 ×", so Multicolor beats Graphics II.
    vdp_legacy_mode_t mode = (reg[VDP_REG_MODE1] & VDP_MODE1_M1)   ? VDP_LEGACY_TEXT
                             : (reg[VDP_REG_MODE1] & VDP_MODE1_M2) ? VDP_LEGACY_MULTICOLOR
                             : (reg[VDP_REG_MODE0] & VDP_MODE0_M3) ? VDP_LEGACY_GRAPHICS_II
                                                                   : VDP_LEGACY_GRAPHICS_I;
    if (legacy) *legacy = mode;
    return &geometries[mode == VDP_LEGACY_TEXT ? VDP_GEOMETRY_TEXT : VDP_GEOMETRY_COMPACT];
}
