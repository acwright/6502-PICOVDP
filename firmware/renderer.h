// The card on two cores (PLAN.md section 3, "Firmware shape").
//
// Core 0 owns the raster: pico9918's VGA driver, cut down to SPEC §3's 262
// line starts (vga/). At each line start it hands the row its buffer and rings
// core 1. Core 1's doorbell interrupt is §3's latch: vdp_latch, as the line
// begins. Core 1's thread is the renderer: it catches up with each latch in
// turn, sends core 0 the sprites left of the line's split through the
// inter-core FIFO, builds the layers and the rest of the sprites, merges,
// publishes status with interrupts held off, and expands the row into a buffer
// that no line start is sending. A row whose build has not finished when its
// line starts shows the last completed row again (§18).
//
// Debug builds add what the debug link reads and drives (docs/DEBUGLINK.md):
// statistics, snapshots, the injection executor, worst-case scenes, and knobs
// that load the line on purpose.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "vdp.h"
#include "vdp_debug.h"

// STAT5 (§6): this firmware's version, BCD. Phase 14 sets the first release's.
#define PICOVDP_VERSION_BCD 0x00

// Core 0, once: the card at power-on, the VGA driver, the sprite job interrupt.
void renderer_init(void);

// Core 0: launch core 1, wait until its latch is in place, start the raster.
void renderer_start(void);

// Core 0, safe mode: the raster only, every row one colour (fault.c).
void renderer_start_safe(uint16_t bgr);

// The watchdog's view of core 1: a count of lines its renderer has taken.
uint32_t renderer_heartbeat(void);

#if PICOVDP_DEBUG

#define RENDERER_HISTOGRAM_BINS 512
#define RENDERER_HISTOGRAM_SHIFT 7    // 128 cycles a bin: 0 to 65,535

// Cycles are the M33's DWT cycle counter, on the core that did the work.
typedef struct renderer_timing {
    uint32_t max;
    uint32_t p999;                    // 99.9th percentile, to the top of its bin
    uint32_t mean;
} renderer_timing_t;

typedef struct renderer_stats {
    uint64_t uptime_us;
    uint32_t clock_hz;
    uint32_t budget;                  // cycles in one display line: two VGA lines at the PIO's rate
    uint32_t rows_built;              // picture rows built
    uint32_t lines_taken;             // latches caught up, rows and blanking
    uint32_t late_rows;               // rows whose line started before their build finished (§18)
    uint32_t latches_merged;          // latches merged into a full ring: rows never built
    uint32_t missed_bells;            // line starts core 1's latch took more than one of at once
    uint32_t journal_overflows;
    uint32_t raster_slips;
    renderer_timing_t latency;        // core 1: latch interrupt to the row's buffer ready
    renderer_timing_t build;          // core 1: catch-up to buffer ready
    // Stage maxima. Core 1: catch-up, building its half, expanding it, the
    // wait for core 0, publishing. Core 0: building and expanding its half.
    uint32_t catch_up_max, half_max, expand_max, wait_max, publish_max;
    uint32_t core0_half_max;
    uint32_t split_mean;              // the column the cores divided rows at
    uint32_t split_rows;              // rows divided between the cores
    uint32_t latch_isr_max;           // core 1's latch interrupt
    uint32_t line_isr_max;            // core 0's line-start interrupt
    uint32_t bus_standins;            // bus stand-in interrupts taken
    uint16_t histogram[RENDERER_HISTOGRAM_BINS];  // latency, saturating
} renderer_stats_t;

void renderer_stats(renderer_stats_t *out, bool reset);

// The card's state as a snapshot records it (docs/DEBUGLINK.md).
typedef struct renderer_state {
    vdp_snapshot_t card;
    bool interrupt;                   // /INT
    uint32_t frame;                   // the raster's frame the state belongs to
    uint32_t scene_frame;             // scene frames begun when it was taken
} renderer_state_t;

// A frame as it went to VGA: 240 rows of indices, the row each was built as,
// and the state when its last row was built.
typedef struct renderer_snapshot {
    uint32_t frame;
    uint8_t source[240];              // the row each line showed: itself, or an earlier one if late
    uint8_t late[240];                // 1 where the row was late
    renderer_state_t state;
    bool state_matches;               // the state is this frame's, not a later one's
    uint8_t rows[240 * 320];
} renderer_snapshot_t;

// Capture raster frame `frame` as it goes to VGA, or the next frame to start
// if 0xffffffff. Any core.
void renderer_capture_arm(uint32_t frame);

// Core 0's thread: wait for the armed capture and the state its last row left.
// The snapshot stays put until renderer_capture_release or the next arm. NULL
// on timeout.
const renderer_snapshot_t *renderer_capture_wait(uint32_t timeout_ms);
void renderer_capture_release(void);

// The bus copy of VRAM: a copy made between two lines on core 1.
bool renderer_vram(uint8_t *out, uint32_t timeout_ms);

// §15 on the card, between two lines.
bool renderer_reset(bool power_on, uint32_t timeout_ms);

// Loads, all off by default (docs/DEBUGLINK.md, LOAD).
typedef struct renderer_load {
    uint32_t bus_rate_hz;             // 0 off: a data write into the palette window this often, on core 1
    uint16_t handicap_first;          // rows [first, last) whose build is padded ...
    uint16_t handicap_last;
    uint16_t handicap_every;          // ... every this many rows ...
    uint32_t handicap_cycles;         // ... to at least this many cycles
} renderer_load_t;
bool renderer_load(const renderer_load_t *load, uint32_t timeout_ms);

// A worst-case scene (scenes.h) from a power-on reset, between lines.
bool renderer_scene(unsigned scene, uint32_t timeout_ms);

// What the scene's program read at each frame's line 250 (scenes.h).
#define RENDERER_SCENE_READS 11
typedef struct renderer_scene_frame {
    uint32_t scene_frame;
    uint8_t reads[RENDERER_SCENE_READS];
} renderer_scene_frame_t;
unsigned renderer_scene_log(renderer_scene_frame_t *out, unsigned max);

// A deliberate fault on core 1 (fault.h's kinds).
void renderer_fault(unsigned kind);

// PROFILE: one row's stages on core 1, interrupts off (profile.h).
struct profile;
bool renderer_profile(uint16_t row, uint32_t iterations, struct profile *out, uint32_t timeout_ms);

#endif
