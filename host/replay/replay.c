// vdp-replay: a docs/TRACE.md trace into the core, in C, against its goldens.
//
//   vdp-replay [--classes] trace.vdpt.gz ...
//
// The host executor of PLAN.md section 4 without Node: tools/replay.mjs's
// replay, on vdp.h and vdp_debug.h alone. A card with no CPU is ticked to each
// event of the trace, as the adapter (host/node/Video.cjs) ticks it — the same
// accumulator deciding when a line starts and what STAT3 b1 reads, the same
// frame built a row at each line start and presented at row 239. Resets,
// writes, reads and checkpoints are fed in at their ticks. Line starts, /INT
// and the values reads return are the card's side: the card makes them, and
// each must be the trace's next event, at its tick. The first that differs
// stops that trace.
//
// Each checkpoint is compared with its golden beside the trace: the index frame
// and all 64 KB of VRAM exactly, and from the JSON the cycle count, the 128
// registers and STAT0. The mode, VRAM hash and text grid are the Node path's.
// So are the frame, settle point and window the trace records for it.
//
//   --classes   also decide each checkpoint's class again: replay to its
//               settle point, apply nothing more, and let the card scan on
//               until the frame is presented (TRACE.md section 4)
//
// Exits 0 if every checkpoint of every trace is exact, 1 if not, 2 on a usage
// or file error.

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "vdp.h"
#include "vdp_debug.h"

#define FRAME_BYTES (VDP_WIDTH * VDP_HEIGHT)

// TRACE.md section 4: the line starts that build a frame's first and last rows.
#define FIRST_ROW_LATCH 261
#define LAST_ROW_LATCH 238

// What the adapter models of Video.ts (host/node/Video.cjs): a cold start at
// display line 0 of Compact, 24 screen lines down; 60 frames a second; STAT3 b1
// over the last fifth of each of a display line's two VGA lines; STAT5 as the
// spec revision.
#define COLD_START_SCREEN_LINE 24
#define FRAMES_PER_SECOND 60
#define HBLANK_FRACTION (640.0 / 800.0)
#define STAT5_SPEC_REVISION 0x05

#define MAX_CHECKPOINTS 32
#define NAME_BYTES 64

// ---- the trace ----

typedef struct event {
    uint64_t tick;  // absolute
    char type;      // X W R L I C
    uint16_t a;     // X: cold; W, R: port; L: screen line; I: level
    uint16_t b;     // X: screen line; W, R: value; L: display line
    int32_t line;   // the event's line in the file, for messages
    char *name;     // C: the checkpoint's name, in the file buffer
    int64_t frame, settle, window;  // C: its annotations, -1 if absent
    int klass;      // C: 1 static, 0 dynamic, -1 absent
} event_t;

typedef struct trace {
    char *text;
    char fixture[NAME_BYTES];
    double frequency;
    event_t *events;
    size_t count;
} trace_t;

static void fail(const char *format, ...) {
    va_list args;
    va_start(args, format);
    fputs("vdp-replay: ", stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
    exit(2);
}

static char *read_gzip(const char *path) {
    gzFile file = gzopen(path, "rb");
    if (!file) fail("cannot open %s", path);
    size_t size = 0, capacity = 1 << 20;
    char *text = malloc(capacity);
    for (;;) {
        if (!text) fail("out of memory reading %s", path);
        if (capacity - size < 65536) {
            capacity *= 2;
            text = realloc(text, capacity);
            continue;
        }
        int got = gzread(file, text + size, 65536);
        if (got < 0) fail("%s is not gzip", path);
        if (got == 0) break;
        size += (size_t)got;
    }
    gzclose(file);
    text[size] = '\0';
    return text;
}

// A byte value: two lowercase hex digits.
static bool parse_byte(const char *field, uint16_t *out) {
    if (strlen(field) != 2) return false;
    unsigned value = 0;
    for (int i = 0; i < 2; i++) {
        char c = field[i];
        if (c >= '0' && c <= '9') value = value * 16 + (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') value = value * 16 + (unsigned)(c - 'a' + 10);
        else return false;
    }
    *out = (uint16_t)value;
    return true;
}

static bool parse_number(const char *field, uint64_t *out) {
    if (!*field) return false;
    uint64_t value = 0;
    for (const char *c = field; *c; c++) {
        if (*c < '0' || *c > '9') return false;
        value = value * 10 + (uint64_t)(*c - '0');
    }
    *out = value;
    return true;
}

// Reads the envelope and every event, and holds the events to the rules the
// replay depends on. tools/lib/trace.mjs checks the rest of TRACE.md section 7.
static void read_trace(const char *path, trace_t *t) {
    memset(t, 0, sizeof *t);
    t->text = read_gzip(path);

    size_t lines = 0;
    for (char *c = t->text; *c; c++) lines += *c == '\n';
    t->events = calloc(lines + 1, sizeof *t->events);
    if (!t->events) fail("out of memory");

    char *cursor = t->text;
    int number = 0;
    bool header = true, ended = false;
    uint64_t tick = 0;
    size_t declared = 0;
    while (*cursor) {
        char *line = cursor;
        char *end = strchr(cursor, '\n');
        if (!end) fail("%s: line %d has no end", path, number + 1);
        *end = '\0';
        cursor = end + 1;
        number++;

        char *fields[16];
        int n = 0;
        for (char *save = NULL, *f = strtok_r(line, " ", &save); f && n < 16; f = strtok_r(NULL, " ", &save)) {
            fields[n++] = f;
        }
        if (number == 1) {
            if (n != 2 || strcmp(fields[0], "vdpt") || strcmp(fields[1], "1")) fail("%s: not a version 1 trace", path);
            continue;
        }
        if (ended) fail("%s: line %d follows the footer", path, number);
        if (header) {
            if (n == 1 && !strcmp(fields[0], "---")) header = false;
            else if (n >= 2 && !strcmp(fields[0], "fixture")) snprintf(t->fixture, sizeof t->fixture, "%s", fields[1]);
            else if (n >= 2 && !strcmp(fields[0], "frequency")) t->frequency = strtod(fields[1], NULL);
            continue;
        }
        uint64_t count;
        if (n == 2 && !strcmp(fields[0], "end") && parse_number(fields[1], &count)) {
            ended = true;
            declared = (size_t)count;
            continue;
        }

        event_t *e = &t->events[t->count];
        uint64_t delta = 0;
        if (n < 3 || strlen(fields[1]) != 1 || !parse_number(fields[0], &delta)) {
            fail("%s: line %d is not an event", path, number);
        }
        tick += delta;
        *e = (event_t){.tick = tick, .type = fields[1][0], .line = number, .frame = -1, .settle = -1, .window = -1, .klass = -1};
        uint64_t value = 0;
        bool ok = false;
        switch (e->type) {
        case 'X':
            ok = n == 4 && (!strcmp(fields[2], "cold") || !strcmp(fields[2], "warm")) && parse_number(fields[3], &value);
            e->a = !strcmp(fields[2], "cold");
            e->b = (uint16_t)value;
            break;
        case 'W':
        case 'R':
            ok = n == 4 && parse_number(fields[2], &value) && value <= 3 && parse_byte(fields[3], &e->b);
            e->a = (uint16_t)value;
            break;
        case 'L': {
            uint64_t display = 0;
            ok = n == 4 && parse_number(fields[2], &value) && parse_number(fields[3], &display);
            e->a = (uint16_t)value;
            e->b = (uint16_t)display;
            break;
        }
        case 'I':
            ok = n == 3 && parse_number(fields[2], &value) && value <= 1;
            e->a = (uint16_t)value;
            break;
        case 'C':
            ok = n >= 4 && parse_number(fields[3], &value) && value == tick;
            e->name = fields[2];
            for (int i = 4; i < n; i++) {
                char *equals = strchr(fields[i], '=');
                if (!equals) ok = false;
                else if (!strncmp(fields[i], "frame=", 6)) e->frame = strtoll(equals + 1, NULL, 10);
                else if (!strncmp(fields[i], "settle=", 7)) e->settle = strtoll(equals + 1, NULL, 10);
                else if (!strncmp(fields[i], "window=", 7)) e->window = strtoll(equals + 1, NULL, 10);
                else if (!strncmp(fields[i], "class=", 6)) e->klass = !strcmp(equals + 1, "static");
            }
            break;
        }
        if (!ok) fail("%s: line %d is not a well-formed %c event", path, number, e->type);
        t->count++;
    }
    if (!ended || declared != t->count) fail("%s: truncated", path);
    if (!t->count || t->events[0].type != 'X' || !t->events[0].a || t->events[0].tick) {
        fail("%s: a version 1 trace starts with a cold reset at tick 0", path);
    }
    if (!(t->frequency > 0)) fail("%s: no frequency", path);
}

// ---- the card, as the adapter drives it ----

typedef struct checkpoint {
    char name[NAME_BYTES];
    uint64_t cycles;
    int64_t frame, settle, window;
    uint8_t indices[FRAME_BYTES];
    uint8_t vram[VDP_VRAM_SIZE];
    uint8_t registers[VDP_REGISTERS];
    uint8_t status;
} checkpoint_t;

typedef struct replay {
    const trace_t *trace;
    vdp_t *card;
    double accumulator, cycles_per_line;
    uint64_t tick;
    uint16_t screen_line;
    uint8_t back[FRAME_BYTES];   // the frame being built
    uint8_t front[FRAME_BYTES];  // the last presented
    int interrupt;               // /INT as last reported

    // Following the trace: the card's events are checked against it.
    bool following;
    size_t cursor;               // the next event the card must make
    bool diverged;
    char why[256];

    // Where the golden frame was built (TRACE.md section 4).
    int64_t frame, ops, latch_settle;
    bool latched, presented;
    int64_t presented_frame, presented_settle, presented_window;

    // A frozen replay: stop following at this frame's first-row latch, this
    // many operations in, and scan on until it is presented.
    bool freeze;
    int64_t freeze_frame, freeze_settle;
    bool frozen, frozen_done;
} replay_t;

static void diverge(replay_t *r, const char *format, ...) {
    if (r->diverged) return;
    r->diverged = true;
    va_list args;
    va_start(args, format);
    vsnprintf(r->why, sizeof r->why, format, args);
    va_end(args);
}

static const char *describe(const event_t *e, char *out, size_t size) {
    switch (e->type) {
    case 'X': snprintf(out, size, "X %s %u", e->a ? "cold" : "warm", e->b); break;
    case 'W':
    case 'R': snprintf(out, size, "%c %u %02x", e->type, e->a, e->b); break;
    case 'L': snprintf(out, size, "L %u %u", e->a, e->b); break;
    case 'I': snprintf(out, size, "I %u", e->a); break;
    default: snprintf(out, size, "C %s", e->name); break;
    }
    return out;
}

// The card made an event: while following, it must be the trace's next, at its
// tick. W and R carry the value the card saw or returned.
static void made(replay_t *r, char type, uint16_t a, uint16_t b) {
    if (!r->following || r->diverged) return;
    event_t made = {.tick = r->tick, .type = type, .a = a, .b = b, .name = ""};
    char expected[96], actual[96];
    if (r->cursor >= r->trace->count) {
        diverge(r, "the trace has ended; the replay made \"%s\" at tick %llu", describe(&made, actual, sizeof actual),
                (unsigned long long)r->tick);
        return;
    }
    const event_t *e = &r->trace->events[r->cursor];
    if (e->type != type || e->a != a || e->b != b || e->tick != r->tick) {
        diverge(r, "event %zu (line %d) is \"%s\" at tick %llu; the replay made \"%s\" at tick %llu", r->cursor + 1, e->line,
                describe(e, expected, sizeof expected), (unsigned long long)e->tick, describe(&made, actual, sizeof actual),
                (unsigned long long)r->tick);
        return;
    }
    r->cursor++;
}

// /INT after an event: the recorder's STAT1 ≠ 0, reported when it changes.
static void sample_interrupt(replay_t *r) {
    int level = vdp_debug_status(r->card, 1) != 0;
    if (level == r->interrupt) return;
    r->interrupt = level;
    made(r, 'I', (uint16_t)level, 0);
}

// §3: a screen line begins, and the row after it is built into the back frame.
static void begin_line(replay_t *r) {
    vdp_line_start(r->card, r->screen_line);
    unsigned row = (r->screen_line + 1u) % VDP_SCREEN_LINES;
    if (row >= VDP_HEIGHT) return;
    vdp_build_line(r->card, r->back + row * VDP_WIDTH);
    if (row == VDP_HEIGHT - 1) memcpy(r->front, r->back, FRAME_BYTES);
}

// Frames, settle points and freezing, as each line start is made.
static void count_line(replay_t *r, uint16_t screen_line) {
    if (screen_line == 0) r->frame++;
    if (!r->following) {
        if (screen_line == LAST_ROW_LATCH && r->frame == r->freeze_frame) r->frozen_done = true;
        return;
    }
    if (screen_line == FIRST_ROW_LATCH) {
        r->latched = true;
        r->latch_settle = r->ops;
        if (r->freeze && r->freeze_frame == r->frame + 1 && r->freeze_settle == r->ops) r->frozen = true;
    }
    if (screen_line == LAST_ROW_LATCH && r->latched) {
        r->presented = true;
        r->presented_frame = r->frame;
        r->presented_settle = r->latch_settle;
        r->presented_window = r->ops - r->latch_settle;
        r->latched = false;
    }
}

static void tick(replay_t *r) {
    r->cycles_per_line = r->trace->frequency / FRAMES_PER_SECOND / VDP_SCREEN_LINES;
    r->tick++;
    r->accumulator++;
    while (r->accumulator >= r->cycles_per_line) {
        r->accumulator -= r->cycles_per_line;
        r->screen_line = (uint16_t)((r->screen_line + 1u) % VDP_SCREEN_LINES);
        begin_line(r);
        made(r, 'L', r->screen_line, vdp_debug_display_line(r->card));
        sample_interrupt(r);
        count_line(r, r->screen_line);
    }
}

static bool horizontal_blanking(const replay_t *r) {
    if (r->cycles_per_line <= 0) return false;
    double vga_line = r->cycles_per_line / 2;
    return fmod(r->accumulator, vga_line) >= vga_line * HBLANK_FRACTION;
}

// The backdrop index, as a reset leaves it (§11).
static uint8_t backdrop(const vdp_t *v) {
    return (uint8_t)(((vdp_debug_register(v, 0x16) & 0x0f) << 4) | (vdp_debug_register(v, 0x07) & 0x0f));
}

static void replay_init(replay_t *r, const trace_t *t, vdp_t *card) {
    memset(r, 0, sizeof *r);
    r->trace = t;
    r->card = card;
    r->following = true;
    vdp_init(card, STAT5_SPEC_REVISION);
    r->screen_line = COLD_START_SCREEN_LINE;
    begin_line(r);
}

static void capture(replay_t *r, const event_t *e, checkpoint_t *c) {
    snprintf(c->name, sizeof c->name, "%s", e->name);
    c->cycles = r->tick;
    c->frame = r->presented_frame;
    c->settle = r->presented_settle;
    c->window = r->presented_window;
    memcpy(c->indices, r->front, FRAME_BYTES);
    for (unsigned a = 0; a < VDP_VRAM_SIZE; a++) c->vram[a] = vdp_debug_vram(r->card, (uint16_t)a);
    for (unsigned i = 0; i < VDP_REGISTERS; i++) c->registers[i] = vdp_debug_register(r->card, i);
    c->status = vdp_debug_status(r->card, 0);
}

// Replays the trace, following it, into `checkpoints`. With r->freeze set, stops
// at the frozen latch instead and scans on; r->front is then the frozen frame.
static size_t run(replay_t *r, checkpoint_t *checkpoints) {
    const trace_t *t = r->trace;
    size_t taken = 0;
    while (r->cursor < t->count && !r->diverged && !r->frozen) {
        size_t index = r->cursor;
        const event_t *e = &t->events[index];
        switch (e->type) {
        case 'X':
            if (index != 0 || !e->a) fail("%s: line %d: only a leading cold reset replays", t->fixture, e->line);
            vdp_reset(r->card, true);
            memset(r->back, backdrop(r->card), FRAME_BYTES);
            r->accumulator = 0;
            r->tick = 0;
            r->screen_line = COLD_START_SCREEN_LINE;
            begin_line(r);
            made(r, 'X', 1, r->screen_line);
            sample_interrupt(r);
            break;
        case 'L':
        case 'I':
            while (r->cursor == index && r->tick < e->tick && !r->diverged) tick(r);
            if (r->cursor == index && !r->diverged) {
                char text[96];
                diverge(r, "event %zu (line %d) is \"%s\"; the replay made nothing by tick %llu", index + 1, e->line,
                        describe(e, text, sizeof text), (unsigned long long)r->tick);
            }
            break;
        default:
            while (r->tick < e->tick && !r->diverged) tick(r);
            if (r->diverged) break;
            if (e->type == 'W') {
                vdp_write(r->card, e->a, (uint8_t)e->b);
                made(r, 'W', e->a, e->b);
                sample_interrupt(r);
                r->ops++;
            } else if (e->type == 'R') {
                vdp_set_hblank(r->card, horizontal_blanking(r));
                made(r, 'R', e->a, vdp_read(r->card, e->a));
                sample_interrupt(r);
                r->ops++;
            } else {
                r->cursor++;
                if (!r->presented) fail("%s/%s: no complete frame before it", t->fixture, e->name);
                if (checkpoints) {
                    if (taken == MAX_CHECKPOINTS) fail("%s: more than %d checkpoints", t->fixture, MAX_CHECKPOINTS);
                    capture(r, e, &checkpoints[taken++]);
                }
            }
            break;
        }
    }
    if (!r->freeze) return taken;
    if (!r->frozen) fail("%s: no first-row latch of frame %lld after %lld operations", t->fixture,
                         (long long)r->freeze_frame, (long long)r->freeze_settle);

    // The program stops here; the card scans on.
    r->following = false;
    uint64_t limit = r->tick + 2 * (uint64_t)ceil(t->frequency / FRAMES_PER_SECOND);
    while (!r->frozen_done && r->tick < limit) tick(r);
    if (!r->frozen_done) fail("%s: frame %lld was never presented", t->fixture, (long long)r->freeze_frame);
    return taken;
}

// ---- the goldens ----

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    if (!file) fail("cannot open %s", path);
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    uint8_t *bytes = malloc((size_t)length + 1);
    if (!bytes || fread(bytes, 1, (size_t)length, file) != (size_t)length) fail("cannot read %s", path);
    fclose(file);
    bytes[length] = 0;
    *size = (size_t)length;
    return bytes;
}

// The number after "key": in a golden JSON, or after its "key": [ for index.
static bool json_number(const char *json, const char *key, int index, long long *out) {
    char quoted[64];
    snprintf(quoted, sizeof quoted, "\"%s\":", key);
    const char *at = strstr(json, quoted);
    if (!at) return false;
    at += strlen(quoted);
    if (index >= 0) {
        at = strchr(at, '[');
        if (!at) return false;
        at++;
        for (int i = 0; i < index; i++) {
            at = strchr(at, ',');
            if (!at) return false;
            at++;
        }
    }
    char *end;
    *out = strtoll(at, &end, 10);
    return end != at;
}

static void problem(char *problems, size_t size, const char *format, ...) {
    size_t used = strlen(problems);
    if (used + 16 >= size) return;
    va_list args;
    va_start(args, format);
    snprintf(problems + used, size - used, "\n          ");
    used = strlen(problems);
    vsnprintf(problems + used, size - used, format, args);
    va_end(args);
}

// Returns the count of differences and sets *first, or 0.
static size_t differs(const uint8_t *a, const uint8_t *b, size_t size, size_t *first) {
    size_t count = 0;
    for (size_t i = 0; i < size; i++) {
        if (a[i] == b[i]) continue;
        if (!count) *first = i;
        count++;
    }
    return count;
}

static bool compare(const char *directory, const checkpoint_t *c, const event_t *noted, char *problems, size_t size) {
    char path[4096];
    size_t length, first = 0, count;
    problems[0] = '\0';

    snprintf(path, sizeof path, "%s/%s.idx.bin", directory, c->name);
    uint8_t *golden = read_file(path, &length);
    if (length != FRAME_BYTES) fail("%s is %zu bytes", path, length);
    if ((count = differs(c->indices, golden, FRAME_BYTES, &first))) {
        problem(problems, size, "index frame: %zu pixel(s), first at (%zu, %zu): %u, the golden %u", count, first % VDP_WIDTH,
                first / VDP_WIDTH, c->indices[first], golden[first]);
    }
    free(golden);

    snprintf(path, sizeof path, "%s/%s.vram.bin", directory, c->name);
    golden = read_file(path, &length);
    if (length != VDP_VRAM_SIZE) fail("%s is %zu bytes", path, length);
    if ((count = differs(c->vram, golden, VDP_VRAM_SIZE, &first))) {
        problem(problems, size, "VRAM: %zu byte(s), first at $%04zx", count, first);
    }
    free(golden);

    snprintf(path, sizeof path, "%s/%s.json", directory, c->name);
    char *json = (char *)read_file(path, &length);
    long long value;
    if (!json_number(json, "cycles", -1, &value)) fail("%s has no cycles", path);
    if ((uint64_t)value != c->cycles) problem(problems, size, "cycles are %llu, the golden's %lld", (unsigned long long)c->cycles, value);
    if (!json_number(json, "status", -1, &value)) fail("%s has no status", path);
    if (value != c->status) problem(problems, size, "STAT0 is $%02x, the golden's $%02llx", c->status, value);
    for (int i = 0; i < VDP_REGISTERS; i++) {
        if (!json_number(json, "registers", i, &value)) fail("%s has no register %d", path, i);
        if (value != c->registers[i]) {
            problem(problems, size, "register $%02x is $%02x, the golden's $%02llx", i, c->registers[i], value);
        }
    }
    free(json);

    if (noted->frame != c->frame) problem(problems, size, "frame is %lld, the trace says %lld", (long long)c->frame, (long long)noted->frame);
    if (noted->settle != c->settle) problem(problems, size, "settle is %lld, the trace says %lld", (long long)c->settle, (long long)noted->settle);
    if (noted->window != c->window) problem(problems, size, "window is %lld, the trace says %lld", (long long)c->window, (long long)noted->window);
    return problems[0] != '\0';
}

// ---- main ----

static const event_t *noted_checkpoint(const trace_t *t, const char *name) {
    for (size_t i = 0; i < t->count; i++) {
        if (t->events[i].type == 'C' && !strcmp(t->events[i].name, name)) return &t->events[i];
    }
    return NULL;
}

int main(int argc, char **argv) {
    bool classes = false;
    int traces = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--classes")) classes = true;
        else if (argv[i][0] == '-') fail("unknown option %s\nusage: vdp-replay [--classes] trace.vdpt.gz ...", argv[i]);
        else traces++;
    }
    if (!traces) fail("usage: vdp-replay [--classes] trace.vdpt.gz ...");

    vdp_t *card = malloc(sizeof *card);
    replay_t *r = malloc(sizeof *r);
    checkpoint_t *checkpoints = malloc(MAX_CHECKPOINTS * sizeof *checkpoints);
    if (!card || !r || !checkpoints) fail("out of memory");

    printf("vdp-replay: into the core\n");
    int failures = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        const char *path = argv[i];
        char directory[4096];
        snprintf(directory, sizeof directory, "%s", path);
        char *slash = strrchr(directory, '/');
        if (slash) *slash = '\0';
        else snprintf(directory, sizeof directory, ".");

        trace_t trace;
        read_trace(path, &trace);
        printf("%s — %s\n", trace.fixture, path);

        replay_init(r, &trace, card);
        size_t taken = run(r, checkpoints);
        if (r->diverged) {
            printf("  FAILED  %s: %s\n", trace.fixture, r->why);
            failures++;
            free(trace.events);
            free(trace.text);
            continue;
        }

        for (size_t c = 0; c < taken; c++) {
            const event_t *noted = noted_checkpoint(&trace, checkpoints[c].name);
            char problems[2048];
            bool bad = compare(directory, &checkpoints[c], noted, problems, sizeof problems);
            const char *klass = "";
            if (classes) {
                replay_init(r, &trace, card);
                r->freeze = true;
                r->freeze_frame = checkpoints[c].frame;
                r->freeze_settle = checkpoints[c].settle;
                run(r, NULL);
                size_t first;
                bool is_static = !differs(r->front, checkpoints[c].indices, FRAME_BYTES, &first);
                klass = is_static ? ", static" : ", dynamic";
                if (noted->klass != (int)is_static) {
                    problem(problems, sizeof problems, "class is %s, the trace says %s", is_static ? "static" : "dynamic",
                            noted->klass < 0 ? "nothing" : noted->klass ? "static" : "dynamic");
                    bad = true;
                }
            }
            if (bad) failures++;
            printf("  %s %s/%s — frame %lld, settle %lld%s%s\n", bad ? "DIFFERS" : "exact  ", trace.fixture, checkpoints[c].name,
                   (long long)checkpoints[c].frame, (long long)checkpoints[c].settle, klass, problems);
        }
        printf("  %zu events\n", trace.count);
        free(trace.events);
        free(trace.text);
    }

    printf(failures ? "%d failure(s)\n" : "every checkpoint replays exactly\n", failures);
    free(checkpoints);
    free(r);
    free(card);
    return failures ? 1 : 0;
}
