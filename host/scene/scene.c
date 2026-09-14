// vdp-scene: the firmware's worst-case scenes (firmware/scenes.h), drawn by the
// core on the host, as the reference the firmware's snapshots of them are
// compared with.
//
//   vdp-scene --list
//   vdp-scene NAME --frame N [--out FILE]   the picture of scene frame N: 76,800 indices
//   vdp-scene NAME --reads FRAMES           what the scene's program reads, frame by frame
//   vdp-scene --check                       every scene is the worst case it claims to be
//
// The program runs as the firmware runs it: set up from a power-on reset at a
// line start of screen line 250; at each later screen line 250, its reads, then
// the next frame's scroll. Frame N's picture is built from scene_frame(N)'s scroll
// on a card set up with nothing else changed, which is what the firmware's rows
// are built from: the program writes nothing else a picture shows.
//
// Exits 0 on success, 1 if --check finds a scene short of its claim, 2 on a
// usage error.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scenes.h"
#include "vdp.h"
#include "vdp_debug.h"

static void port_write(void *context, unsigned port, uint8_t value) {
    vdp_write(context, port, value);
}

static uint8_t port_read(void *context, unsigned port) {
    return vdp_read(context, port);
}

static void usage(void) {
    fputs("usage: vdp-scene --list | NAME --frame N [--out FILE] | NAME --reads FRAMES | --check\n", stderr);
    exit(2);
}

static int find(const char *name) {
    int index = scene_find(name);
    if (index < 0) {
        fprintf(stderr, "vdp-scene: no scene %s\n", name);
        exit(2);
    }
    return index;
}

// Set up at screen line 250, as the firmware does.
static void set_up(vdp_t *v, const scene_t *s, const scene_port_t *port) {
    vdp_init(v, 0x00);
    for (unsigned line = 0; line <= SCENE_LINE; line++) vdp_line_start(v, (uint16_t)line);
    vdp_reset(v, true);
    scene_setup(s, port);
}

static void frame(const scene_t *s, unsigned n, uint8_t *indices) {
    static vdp_t v;
    const scene_port_t port = {port_write, port_read, &v};
    set_up(&v, s, &port);
    scene_frame(s, n, &port);
    // Rows 0-239, each built as the screen line before it begins.
    vdp_line_start(&v, VDP_SCREEN_LINES - 1);
    vdp_build_line(&v, indices);
    for (unsigned row = 1; row < VDP_HEIGHT; row++) {
        vdp_line_start(&v, (uint16_t)(row - 1));
        vdp_build_line(&v, indices + row * VDP_WIDTH);
    }
}

static void reads(const scene_t *s, unsigned frames) {
    static vdp_t v;
    const scene_port_t port = {port_write, port_read, &v};
    set_up(&v, s, &port);
    uint8_t row[VDP_WIDTH];
    unsigned count = 0;
    uint16_t line = SCENE_LINE;
    while (count < frames) {
        line = (uint16_t)((line + 1) % VDP_SCREEN_LINES);
        vdp_line_start(&v, line);
        vdp_build_line(&v, row);
        if (line != SCENE_LINE) continue;
        uint8_t values[SCENE_READS];
        scene_reads(&port, values);
        printf("%u", count);
        for (unsigned i = 0; i < SCENE_READS; i++) printf(" %02x", values[i]);
        printf("\n");
        scene_frame(s, ++count, &port);
    }
}

// The claims scenes.h makes: in the band, SPRLIMIT sprites drawn and the rest
// dropped, every drawn sprite colliding, and both layers drawn on.
static bool check(const scene_t *s) {
    static vdp_t v;
    const scene_port_t port = {port_write, port_read, &v};
    set_up(&v, s, &port);
    uint8_t row[VDP_WIDTH];
    bool ok = true;
    unsigned span = 16u << s->magnified, band = 0;
    for (unsigned n = 0; n < 4; n++) {
        scene_frame(s, n, &port);
        for (unsigned line = 0; line < VDP_SCREEN_LINES; line++) {
            // Status cleared, so this line's overflow and collisions are its own.
            memset(v.collision_map, 0, sizeof v.collision_map);
            v.stat0 = 0;
            v.frame_events = 0;
            vdp_line_start(&v, (uint16_t)line);
            const unsigned row_number = (line + 1) % VDP_SCREEN_LINES;
            const bool in_band = row_number >= 100 && row_number < 100 + span;
            const unsigned listed = v.sprite_count;
            const bool overflow = (v.stat0 & 0x40) != 0;
            vdp_build_line(&v, row);
            if (!in_band) continue;
            band++;
            unsigned mapped = 0;
            for (unsigned b = 0; b < 8; b++) mapped += (unsigned)__builtin_popcount(v.collision_map[b]);
            bool detailed_ok = !s->detailed || mapped == s->limit;
            if (listed != s->limit || !overflow || !(v.stat0 & 0x60) || !detailed_ok) {
                if (ok) {
                    printf("  %s, row %u: %u sprites listed, overflow %u, STAT0 $%02x, %u in the map\n", s->name,
                           row_number, listed, overflow, v.stat0, mapped);
                }
                ok = false;
            }
        }
    }
    if (band != 4 * span) ok = false;
    return ok;
}

int main(int argc, char **argv) {
    if (argc < 2) usage();
    if (!strcmp(argv[1], "--list")) {
        for (unsigned i = 0; i < scene_count(); i++) printf("%s\n", scene_at(i)->name);
        return 0;
    }
    if (!strcmp(argv[1], "--check")) {
        unsigned failed = 0;
        for (unsigned i = 0; i < scene_count(); i++) {
            if (!check(scene_at(i))) failed++;
        }
        printf("%u scenes, %u short of the worst case they claim\n", scene_count(), failed);
        return failed ? 1 : 0;
    }
    if (argc < 4) usage();
    const scene_t *s = scene_at((unsigned)find(argv[1]));
    if (!strcmp(argv[2], "--frame")) {
        static uint8_t indices[VDP_WIDTH * VDP_HEIGHT];
        frame(s, (unsigned)strtoul(argv[3], NULL, 10), indices);
        FILE *out = stdout;
        if (argc >= 6 && !strcmp(argv[4], "--out")) out = fopen(argv[5], "wb");
        if (!out || fwrite(indices, 1, sizeof indices, out) != sizeof indices) {
            fputs("vdp-scene: cannot write the frame\n", stderr);
            return 2;
        }
        if (out != stdout) fclose(out);
        return 0;
    }
    if (!strcmp(argv[2], "--reads")) {
        reads(s, (unsigned)strtoul(argv[3], NULL, 10));
        return 0;
    }
    usage();
}
