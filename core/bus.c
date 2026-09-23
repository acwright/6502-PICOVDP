// The bus side: four ports (§4), the register file (§5) and VRAM (§7).
//
// Everything here runs as the CPU accesses the card — on the RP2350, in core
// 1's bus interrupt — and touches only the bus side. What the render side needs
// to know of it is left in the journal for the latch (line.c).

#include "vdp_internal.h"

#include <string.h>

// §7: a byte of VRAM. The bus copy takes it at once, so either port reads it
// back at once; the journal carries it to the render copy at the catch-up of
// the next latch (§3). While the journal is full the page is marked instead,
// and that catch-up copies it whole. Every write to VRAM comes through here —
// both data ports, reset's palette and a debugger's pokes — because the palette
// snoop (§11) is a property of the memory, not of who wrote to it.
void VDP_HOT(vdp_poke)(vdp_t *v, uint16_t address, uint8_t value) {
    v->vram[address] = value;
    uint32_t tail = v->journal_tail;
    if (tail - v->journal_head < VDP_JOURNAL_ENTRIES) {
        v->journal_address[tail & (VDP_JOURNAL_ENTRIES - 1)] = address;
        v->journal_value[tail & (VDP_JOURNAL_ENTRIES - 1)] = value;
        v->journal_tail = tail + 1;
    } else {
        v->dirty_pages |= UINT64_C(1) << (address >> VDP_VRAM_PAGE_SHIFT);
    }
}

// A block of constant data, written to the bus copy at once. The render side
// takes it whole when its replay of the journal reaches the place it was
// written, so it lands in order among the single writes around it, and a late
// catch-up takes exactly what a prompt one does. A ring that is full marks the
// pages instead, as a full journal does.
void VDP_HOT(vdp_bulk_write)(vdp_t *v, uint16_t address, const uint8_t *bytes, unsigned length) {
    if (length == 0) return;
    memcpy(v->vram + address, bytes, length);
    uint32_t tail = v->bulk_tail;
    if (tail - v->bulk_head < VDP_BULK_ENTRIES) {
        v->bulk[tail & (VDP_BULK_ENTRIES - 1)] =
            (vdp_bulk_t){.bytes = bytes, .at = v->journal_tail, .address = address, .length = (uint16_t)length};
        v->bulk_tail = tail + 1;
        return;
    }
    unsigned first = address >> VDP_VRAM_PAGE_SHIFT;
    unsigned last = (address + length - 1u) >> VDP_VRAM_PAGE_SHIFT;
    for (unsigned page = first; page <= last; page++) v->bulk_pages |= UINT64_C(1) << page;
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
    // §7: every write to FONT is a command, even of the value it holds. A
    // debugger's write is one too, as it is in Video.ts.
    if (home == VDP_REG_FONT) vdp_font_command(v, value);
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

uint8_t VDP_HOT(vdp_read)(vdp_t *v, unsigned port) {
    unsigned pair = (port >> 1) & 1;
    vdp_port_t *p = &v->port[pair];
    // Any access to a pair's data port, and any read of its status port, resets
    // that pair's flip-flop (§4).
    p->second = false;
    if (port & 1) {
        // Port A's selector is STATSEL_A at $0F, port B's STATSEL_B at $0E (§5).
        // Each port reads through its own, so a handler on one cannot move what
        // the other sees (§6).
        return vdp_status_read(v, v->reg[pair ? VDP_REG_STATSEL_B : VDP_REG_STATSEL_A]);
    }
    // Read VC_DATA: return the prefetch; fetch the byte at the pointer into it;
    // advance.
    uint8_t value = p->prefetch;
    p->prefetch = v->vram[p->pointer];
    advance(v, p);
    return value;
}

// §2: the read program answers from a word staged before the read, one byte a
// port in A1:A0's order — data A, status A, data B, status B — each what
// vdp_read would return now. Nothing here changes the card.
uint32_t VDP_HOT(vdp_staged)(const vdp_t *v) {
    return (uint32_t)v->port[0].prefetch |
           (uint32_t)vdp_status_peek(v, v->reg[VDP_REG_STATSEL_A]) << 8 |
           (uint32_t)v->port[1].prefetch << 16 |
           (uint32_t)vdp_status_peek(v, v->reg[VDP_REG_STATSEL_B]) << 24;
}

// A read the pins have already answered with `served`, from a staged word. When
// that is what a read now returns, this is vdp_read exactly. When the card has
// moved on since the word was staged — a flag set by a latch the restage had
// not yet reached — the read acknowledges only what it showed (§6): a flag the
// CPU never saw is not cleared. A data read moves its port on the same way
// whatever it returned. Returns what vdp_read would have, so the caller can
// count reads that were served stale.
uint8_t VDP_HOT(vdp_read_served)(vdp_t *v, unsigned port, uint8_t served) {
    if (!(port & 1)) return vdp_read(v, port);
    unsigned pair = (port >> 1) & 1;
    unsigned select = v->reg[pair ? VDP_REG_STATSEL_B : VDP_REG_STATSEL_A];
    uint8_t now = vdp_status_peek(v, select);
    if (now == served) return vdp_read(v, port);
    v->port[pair].second = false;
    vdp_status_acknowledge(v, select, served);
    return now;
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
