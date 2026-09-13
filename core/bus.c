// The bus side: four ports (§4), the register file (§5) and VRAM (§7).
//
// Everything here runs as the CPU accesses the card — on the RP2350, in core
// 1's bus interrupt — and touches only the bus side. What the render side needs
// to know of it is left in the journal for the latch (line.c).

#include "vdp_internal.h"

#include <string.h>

// §7: a byte of VRAM. The bus copy takes it at once, so either port reads it
// back at once; the journal carries it to the render copy at the next latch
// (§3). Once the journal is full for this line, the page is marked instead and
// the latch copies it whole. Every write to VRAM comes through here — both data
// ports, reset's palette and a debugger's pokes — because the palette snoop
// (§11) is a property of the memory, not of who wrote to it.
void VDP_HOT(vdp_poke)(vdp_t *v, uint16_t address, uint8_t value) {
    v->vram[address] = value;
    if (v->journal_count < VDP_JOURNAL_ENTRIES) {
        v->journal_address[v->journal_count] = address;
        v->journal_value[v->journal_count] = value;
        v->journal_count++;
    } else {
        v->dirty_pages |= UINT64_C(1) << (address >> VDP_VRAM_PAGE_SHIFT);
    }
}

// §5. Seven bits of register number, so $08-$7F are always live. A reserved
// register stores what is written and does nothing more.
void VDP_HOT(vdp_register_write)(vdp_t *v, unsigned index, uint8_t value) {
    unsigned home = vdp_register_home(index);
    v->reg[home] = value;
    // §14: MODE1 b5 and IRQEN b0 are one bit under two names. The byte-wide
    // aliases give a register one home; a single bit cannot have one without a
    // branch in every read, so the two copies are kept equal on the way in.
    if (home == VDP_REG_MODE1) {
        v->reg[VDP_REG_IRQEN] = (uint8_t)((v->reg[VDP_REG_IRQEN] & ~VDP_IRQ_VBLANK) |
                                          ((value & VDP_MODE1_IE) ? VDP_IRQ_VBLANK : 0));
    } else if (home == VDP_REG_IRQEN) {
        v->reg[VDP_REG_MODE1] = (uint8_t)((v->reg[VDP_REG_MODE1] & ~VDP_MODE1_IE) |
                                          ((value & VDP_IRQ_VBLANK) ? VDP_MODE1_IE : 0));
    }
    // PALBASE needs nothing here: the latch sees it has moved and re-reads the
    // window (§11).
}

// §4: after each data-port access the pointer moves by the signed stride in
// VINC, carrying into the bank bits and wrapping at 64 KB. The carry is the
// port's own; VBANK never follows it.
static inline void advance(vdp_t *v, vdp_port_t *p) {
    p->pointer = (uint16_t)(p->pointer + (int8_t)v->reg[VDP_REG_VINC]);
}

// §4's command protocol: a payload, then a command byte.
static void VDP_HOT(command)(vdp_t *v, vdp_port_t *p, uint8_t value) {
    if (!p->second) {
        p->payload = value;
        p->second = true;
        return;
    }
    p->second = false;

    if (value & VDP_CMD_REGISTER) {
        // A register write moves neither port's pointer nor prefetch.
        vdp_register_write(v, value & VDP_REGISTER_MASK, p->payload);
        return;
    }

    // Bits 13:0 from the pair, 15:14 from VBANK, sampled now: a later VBANK
    // write does not move a pointer already set.
    p->pointer = (uint16_t)(((v->reg[VDP_REG_VBANK] & 0x03) << 14) | ((value & VDP_CMD_ADDRESS) << 8) |
                            p->payload);
    p->read_mode = !(value & VDP_CMD_WRITE);
    if (p->read_mode) {
        // Set a read address: fetch the byte at the pointer into the prefetch;
        // advance. Setting a write address does nothing more.
        p->prefetch = v->vram[p->pointer];
        advance(v, p);
    }
}

// §6: the status register a port's STATSEL names. Reading any of them resets
// the port's flip-flop (§4), which the caller has done.
//
// Phase 3: the three constant registers. The flags, latches, line and blanking
// bits, overflow index and collision map are Phase 4's, and read 0 until then.
static uint8_t status(const vdp_t *v, unsigned select) {
    switch (select) {
    case 4:
        return VDP_STAT_IDENTIFICATION;
    case 5:
        return v->version;
    case 6:
        return VDP_STAT_CAPABILITIES;
    default:
        return 0;
    }
}

uint8_t VDP_HOT(vdp_read)(vdp_t *v, unsigned port) {
    unsigned pair = (port >> 1) & 1;
    vdp_port_t *p = &v->port[pair];
    // Any access to a pair's data port, and any read of its status port, resets
    // that pair's flip-flop (§4).
    p->second = false;
    if (port & 1) {
        // Port A's selector is STATSEL_A at $0F, port B's STATSEL_B at $0E (§5).
        unsigned select = v->reg[pair ? VDP_REG_STATSEL_B : VDP_REG_STATSEL_A] & VDP_STATSEL_MASK;
        return status(v, select);
    }
    // Read VC_DATA: return the prefetch; fetch the byte at the pointer into it;
    // advance.
    uint8_t value = p->prefetch;
    p->prefetch = v->vram[p->pointer];
    advance(v, p);
    return value;
}

void VDP_HOT(vdp_write)(vdp_t *v, unsigned port, uint8_t value) {
    vdp_port_t *p = &v->port[(port >> 1) & 1];
    if (port & 1) {
        command(v, p, value);
        return;
    }
    // Write VC_DATA: store at the pointer; load the written byte into the
    // prefetch; advance.
    p->second = false;
    vdp_poke(v, p->pointer, value);
    p->prefetch = value;
    advance(v, p);
}
