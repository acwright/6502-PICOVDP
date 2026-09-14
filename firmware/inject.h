// The injection executor (PLAN.md section 4, docs/DEBUGLINK.md's INJECT).
//
// A trace's program side, streamed from the host: each operation addressed by
// the (frame, screen line) it fell in, and applied after that line's latch as
// if from the bus, with every read and /INT compared with what the trace says.
// No clock: the raster only paces it.
//
// While an injection runs, core 1 takes each latch in its thread rather than
// in the interrupt, in the order a trace replay takes them (TRACE.md section
// 6): the latch, the row it starts built and its status published, then the
// line's operations, then the next latch. So a read sees exactly what the
// replay's does, however long a build takes. Outside an injection the latch is
// the interrupt's, as the bus will need.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "renderer.h"

// Stream records (docs/DEBUGLINK.md).
#define INJECT_LINE 0x01        // u16 frame, u16 screen line, u8 /INT after the latch
#define INJECT_COLD 0x02        // u16 screen line
#define INJECT_END 0x03
#define INJECT_OP 0x80          // | read 0x40 | /INT after 0x20 | port; then the value written or expected

enum { INJECT_IDLE, INJECT_ARMED, INJECT_RUNNING, INJECT_ENDED, INJECT_FAILED };

enum { MISMATCH_READ, MISMATCH_INT_AFTER_OP, MISMATCH_INT_AFTER_LATCH, MISMATCH_LATE_RECORD, MISMATCH_BAD_RECORD };

typedef struct inject_mismatch {
    uint32_t op;                // operations applied before it
    uint16_t frame, screen_line;
    uint8_t kind, port, expected, got, select;
} inject_mismatch_t;

#define INJECT_LOG 32

typedef struct inject_status {
    uint8_t state;
    uint32_t base_frame;        // the raster frame the cold reset fell in: trace frame 0
    uint16_t frame, screen_line;  // the trace line last applied
    uint32_t ops, reads, stat5_reads, mismatches;
    uint32_t space;             // stream bytes the ring can take
    uint32_t buffered;
    uint32_t late_rows;         // late rows during the injection, from the renderer's count
    renderer_state_t end_state; // at END
    uint8_t logged;
    inject_mismatch_t log[INJECT_LOG];
} inject_status_t;

// ---- core 0's link thread ----

// Start an injection; capture raster frame (cold frame + capture_frame) as it
// goes to VGA, unless capture_frame is 0xffff. False if one is running.
bool inject_begin(uint16_t capture_frame);
// Stream bytes, whole records only. Returns how many were taken.
uint32_t inject_data(const uint8_t *bytes, uint32_t count);
void inject_abort(void);
void inject_status(inject_status_t *out);

// ---- core 1 ----

bool inject_active(void);                 // the thread takes latches (latch interrupt and thread)
bool inject_start_requested(void);        // thread: switch on at the next line
bool inject_stop_requested(void);         // thread: switch off once caught up
void inject_set_active(bool active);      // thread, interrupts off
void inject_before_latch(vdp_t *v, uint32_t frame, uint16_t screen_line);
void inject_after_line(vdp_t *v, uint32_t frame, uint16_t screen_line);
