// Driving a card the way a program does, for tests/unit.

#pragma once

#include <stdlib.h>

#include "vdp.h"
#include "vdp_debug.h"

// A card, power-on reset. vdp_t is about 135 KB: on the heap, not the stack.
static vdp_t *new_card(void) {
    vdp_t *v = malloc(sizeof *v);
    if (v == NULL) abort();
    vdp_init(v, 0x04);
    return v;
}

// Two writes to a pair's command port (§4): payload, then command byte.
static void command(vdp_t *v, unsigned pair, uint8_t payload, uint8_t byte) {
    vdp_write(v, 2 * pair + 1, payload);
    vdp_write(v, 2 * pair + 1, byte);
}

static void set_reg(vdp_t *v, unsigned index, uint8_t value) {
    command(v, 0, value, (uint8_t)(0x80 | index));
}

// Point a pair at any of the 64 KB: VBANK, then the address command.
static void point_at(vdp_t *v, unsigned pair, uint16_t address, bool write) {
    set_reg(v, 0x08, (uint8_t)(address >> 14));
    command(v, pair, (uint8_t)address, (uint8_t)(((address >> 8) & 0x3f) | (write ? 0x40 : 0)));
}

static void store(vdp_t *v, uint16_t address, const uint8_t *bytes, size_t count) {
    point_at(v, 0, address, true);
    for (size_t i = 0; i < count; i++) vdp_write(v, 0, bytes[i]);
}
