// PROFILE (docs/DEBUGLINK.md): each stage of one row, run again and again on
// core 1 with interrupts off and timed with the DWT cycle counter, as Phase 1's
// stage suite timed the spike (docs/results/phase-01.md). Debug builds only;
// the raster's latches are held off while it runs.

#include "profile.h"

#include <string.h>

#include "hardware/structs/m33.h"
#include "hardware/sync.h"

#include "vdp_internal.h"

static inline uint32_t cycles(void) {
    return m33_hw->dwt_cyccnt;
}

typedef struct stage {
    uint32_t min, max;
} stage_t;

#define TIME(stage, body)                                        \
    do {                                                         \
        const uint32_t t0_ = cycles();                           \
        body;                                                    \
        const uint32_t d_ = cycles() - t0_;                      \
        if (d_ < (stage).min) (stage).min = d_;                  \
        if (d_ > (stage).max) (stage).max = d_;                  \
    } while (0)

// On the card itself: everything timed writes only the render side's scratch,
// which is the thread's between lines, and the next catch-up evaluates again.
void profile_row(vdp_t *v, uint16_t row, uint32_t iterations, profile_t *out) {
    memset(out, 0, sizeof *out);
    stage_t s[PROFILE_STAGES];
    for (unsigned i = 0; i < PROFILE_STAGES; i++) s[i] = (stage_t){UINT32_MAX, 0};
    const uint16_t render_screen_line = v->render_screen_line;
    vdp_t *const card = v;
#define scratch (*card)

    vdp_legacy_mode_t legacy;
    const vdp_geometry_t *g = vdp_geometry(scratch.render_reg, &legacy);
    const uint16_t display = vdp_display_line_of(row, g);
    vdp_half_t *h = &scratch.half;
    uint8_t *picture = h->line + VDP_LINE_SLACK + g->origin_x;
    uint8_t *levels = h->level + VDP_LINE_SLACK + g->origin_x;
    const int middle = (g->width / 2) & ~7;
    uint16_t rgb[2 * VDP_WIDTH];

    const uint32_t off = save_and_disable_interrupts();
    const uint32_t probe0 = cycles(), probe1 = cycles();
    out->probe = probe1 - probe0;
    for (uint32_t n = 0; n < iterations; n++) {
        scratch.render_screen_line = row;
        TIME(s[PROFILE_EVALUATE], vdp_sprites_evaluate(&scratch));
        TIME(s[PROFILE_LAYER0], {
            memset(h->level, 0, VDP_LINE_BYTES);
            vdp_draw_layer(&scratch, 0, display, g, legacy, picture, levels, 0, g->width);
        });
        TIME(s[PROFILE_LAYER1], vdp_draw_layer(&scratch, 1, display, g, VDP_LEGACY_NONE, picture, levels, 0, g->width));
        TIME(s[PROFILE_ROW], vdp_build_half(&scratch, h, 0, VDP_WIDTH));
        TIME(s[PROFILE_EXPAND], vdp_expand_half(&scratch, h, rgb));
        TIME(s[PROFILE_LEFT], vdp_build_half(&scratch, h, 0, middle));
        TIME(s[PROFILE_RIGHT], vdp_build_half(&scratch, h, middle, VDP_WIDTH));
        TIME(s[PROFILE_SPLIT_CHOOSE], (void)vdp_split_choose(&scratch));
    }
    restore_interrupts(off);

    for (unsigned i = 0; i < PROFILE_STAGES; i++) {
        out->min[i] = s[i].min;
        out->max[i] = s[i].max;
    }
    out->sprites = scratch.sprite_count;
    out->row = row;
    v->render_screen_line = render_screen_line;
    vdp_sprites_evaluate(v);
#undef scratch
}
