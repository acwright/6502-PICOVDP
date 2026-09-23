The Bench
=========

How the PICO9918 PRO is mounted, wired, driven and watched while this repo's
firmware is brought up (PLAN.md section 5, Phases 9 to 13). The PRO sits in the
TMS9918A's 40-pin DIP position on a breadboard, an Arduino Nano drives its four
ports, and the Mac drives the Nano. Nothing here is fabricated: every connection
is a jumper wire.

The wiring is also drawn, as `bench/hardware/picovdp-bench.kicad_sch`. The
schematic and the table in section 3 are the same netlist; keep them in step.

In this document, **§** means a section of SPEC.md.

---

Contents
--------

1. [Parts](#1-parts)
2. [Preparing the PRO](#2-preparing-the-pro)
3. [Wiring](#3-wiring)
4. [Why the Pins Fall Where They Do](#4-why-the-pins-fall-where-they-do)
5. [The Nano Harness](#5-the-nano-harness)
6. [Video Capture](#6-video-capture)
7. [Bring-Up Order](#7-bring-up-order)
8. [What Phase 9 Proves](#8-what-phase-9-proves)
9. [The Schematic](#9-the-schematic)

---

1. Parts
--------

| Part | Status |
|---|---|
| PICO9918 PRO v2.0, HDMI dongle, FFC cable | on hand |
| Arduino Nano (ATmega328P, 5 V, 16 MHz) | on hand |
| Arduino Mega 2560 | on hand, fallback |
| HDMI capture card | on hand |
| Raspberry Pi Pico 2 | on hand, Phases 1 and 8 |
| One 0.1″ male header pin, for `MDE1` | needed |
| 8 × 220 Ω resistors | needed |
| 2 × 10 kΩ resistors | needed |
| Breadboard or DIP-40 socket, jumper wires | needed |
| 74AHCT125 (or another 74HCT-family gate) and a 100 nF cap | optional, Phase 13 |

Two USB cables, both to the Mac: USB-C to the PRO, USB-B to the Nano.

The Nano was chosen over the Mega 2560 for a cleaner breadboard. It is the same
16 MHz AVR, so strobe timing and `/INT` capture resolution are unchanged. What
it gives up is RAM (2 KB against 8 KB), a free 8-bit port for data, and spare
input-capture timers; section 4 accounts for each. The Mega stays on hand as the
fallback.

---

2. Preparing the PRO
--------------------

**Back up the stock flash first.** Hold the button down while plugging the USB-C
in, then `picotool save -a`. The stock firmware does not enumerate on USB, so
BOOTSEL is the only way in and the button is the only way to BOOTSEL. Keep the
image outside the repo. Everything in Phase 9 runs on that stock firmware, and
Phase 10 overwrites it.

This bench's copy is in `~/Developer/Backups/pico9918-pro/`, with a SHA-256 and
the `picotool info -a` output beside it (`docs/results/phase-09.md`).

**Fit the `MDE1` pin.** The `MDE1` pad — the TMS9918's pin 11 position — ships
bare, because MODE1 is not a TMS9918 signal. Solder one 0.1″ male header pin
into it. Without it, port B (`$9C02`/`$9C03`) is unreachable, on the bench and in
the AC6502 alike.

No other modification is needed.

**Leave pin 33 unconnected.** On a real TMS9918A that pin is +5 V. Here the PRO
runs from its own USB-C, which also carries the debug link from Phase 10 onward.
The Nano runs from its own USB. **Only ground is common**: tie the Nano's `GND`
to the PRO's pin 12 through the breadboard's ground rail.

---

3. Wiring
---------

TI numbers the TMS9918 data bus backwards: **CD0 is the most significant bit and
CD7 the least**. pico9918 samples GPIO 14 (CD7) as bit 0. Get this wrong and
every byte arrives bit-reversed.

The Nano's `A0`–`A5` are header pins on its analogue side, used here as digital
I/O. They have nothing to do with the CPU address lines that MODE and MODE1
stand for.

| PRO label | TMS9918 pin | Signal | Nano pin | AVR port bit | Through |
|---|:--:|---|:--:|:--:|---|
| CD7 | 17 | data bit 0 (LSB) | D2 | PD2 | 220 Ω (R1) |
| CD6 | 18 | data bit 1 | D3 | PD3 | 220 Ω (R2) |
| CD5 | 19 | data bit 2 | D4 | PD4 | 220 Ω (R3) |
| CD4 | 20 | data bit 3 | D5 | PD5 | 220 Ω (R4) |
| CD3 | 21 | data bit 4 | D6 | PD6 | 220 Ω (R5) |
| CD2 | 22 | data bit 5 | D7 | PD7 | 220 Ω (R6) |
| CD1 | 23 | data bit 6 | D9 | PB1 | 220 Ω (R7) |
| CD0 | 24 | data bit 7 (MSB) | D10 | PB2 | 220 Ω (R8) |
| CSW | 14 | `/CSW` | A0 | PC0 | 10 kΩ (R9) to +5 V |
| CSR | 15 | `/CSR` | A1 | PC1 | 10 kΩ (R10) to +5 V |
| MDE | 13 | MODE = CPU A0 | A2 | PC2 | — |
| MDE1 | 11 (fitted pin) | MODE1 = CPU A1 | A3 | PC3 | — |
| RST | 34 | `/RESET` | A4 | PC4 | — |
| INT | 16 | `/INT` (10 k pull-up on the PRO) | D8 | PB0 = ICP1 | — |
| — | — | frame sync (optional, Phase 13) | A5 | PC5 = PCINT13 | 74AHCT125 |
| GND | 12 | ground | GND | — | — |
| +5V | 33 | **leave unconnected** | — | — | — |

The +5 V for R9 and R10 comes from the Nano's own `5V` pin.

### The port decode

`MODE1:MODE` is the CPU's `A1:A0`, so the four ports are:

| MODE1 | MODE | Address | Port |
|:--:|:--:|---|---|
| 0 | 0 | `$9C00` | data |
| 0 | 1 | `$9C01` | address / register |
| 1 | 0 | `$9C02` | port B data |
| 1 | 1 | `$9C03` | port B command |

---

4. Why the Pins Fall Where They Do
----------------------------------

**The data bus is split.** `D0` and `D1` carry the USB serial link, and
`PB6`/`PB7` and `PC6` hold the crystal and reset, so the 328P has no free 8-bit
port. Six data bits go on PORTD, two on PORTB. Control stays on PORTC alone, so
each strobe edge is still a single port write:

- **On a write,** both data ports are set before `/CSW` falls.
- **On a read,** both are sampled while `/CSR` is low, one instruction (62.5 ns)
  apart, and the PRO holds the bus throughout.

**`/INT` must be D8.** `PB0` is `ICP1`, the 328P's only input-capture pin. Timer
1 timestamps `/INT` edges there to 62.5 ns. An optional frame-sync tap is
timestamped instead from Timer 1's count inside a pin-change interrupt, which is
good to a few µs against a 63.6 µs line.

**`D11`–`D13` stay free.** `D13`'s on-board LED is the harness's status light.

**The 220 Ω resistors are protection, not level shifting.** The socket side of
the PRO is 5 V logic — a 74HC245 drives out and a 5 V-tolerant 74LVC245 receives
— so the Nano needs no shifter. The resistors limit the current if a harness bug
drives the data lines while the PRO is driving them.

**The 10 kΩ pull-ups matter more than they look.** They hold both strobes high
while the Nano's pins float in reset. The host opening the serial port pulses
DTR, which resets the Nano; without the pull-ups the PRO would see stray
accesses on every connect. Phase 9 tests exactly this, 100 times over.

**The frame-sync tap needs HCT.** A 3.3 V source into the Nano's 5 V input is
marginal: the AVR's `VIH` is 0.6 × VCC = 3.0 V. An HCT-family input threshold is
2.0 V, which makes 3.3 V an unambiguous high. Where that 3.3 V source comes from
is open — see below.

### The frame-sync tap is optional and its source is undecided

It was to be the VGA dongle's `VSYNC` pin. The HDMI dongle has no analogue sync
to clip onto, so if Phase 13 wants a raster reference independent of `/INT`,
this repo's firmware must bring one out on a spare PRO GPIO. Phase 13's latch
and interrupt measurements are all relative to `/INT`, which the input capture
already timestamps, so the tap may simply not be fitted. Decided in Phase 13.

---

5. The Nano Harness
-------------------

`bench/nano` is a PlatformIO project. Its environments are `nanoatmega328new`,
for the current bootloader, and `nanoatmega328`, for clones with the old one.

```
pio run                      build
pio run -t upload            upload (current bootloader)
pio run -e nanoatmega328 -t upload    clones with the old bootloader
```

**The Nano on this bench is a CH340 clone with the current bootloader**, so the
default `nanoatmega328new` environment is the right one and the `nanoatmega328`
fallback is not needed. It enumerates as `/dev/cu.usbserial-*`, not
`/dev/cu.usbmodem*` — that last form belongs to the PRO's debug link.

It speaks a framed protocol at **1,000,000 baud**. The 328P's 16 MHz clock hits
that rate exactly; the Nano's USB bridge (FT232RL on genuine boards, CH340 on
most clones) and its macOS driver must carry it too. The fallback is 500,000
baud, which is also exact.

**1,000,000 baud is confirmed** on this board, through the CH340 on macOS, at
the device level and through the host library alike. The fallback is not needed.

The harness is a small **bus-script runner**, not a byte relay, because anything
timing-critical has to happen on the Nano: USB serial latency is milliseconds.

**Primitives.** Write or read a port; block write or block read; pulse
`/RESET`; timestamp `/INT` edges.

**Timing profiles.**

| Profile | Strobe width | Accesses apart |
|---|---|---|
| `6502-1mhz` | ≈ 500 ns | ≥ 4 µs |
| `6502-2mhz` | ≈ 250 ns | ≥ 2 µs |
| `fastest` | as fast as the 328P drives the pins | — |

Strobe width, data setup and hold can also be set directly, for margin sweeps.

**Scripts** run locally with deterministic timing: wait for `/INT` (with
timeout), delay µs, write, read and compare, record a timestamp, loop. A script
is held in RAM, so it is capped at 256 bytes — ample for Phase 13's loops, which
are a handful of operations. Scanline-interrupt tests run this way, reacting
within microseconds.

**Invariants the harness enforces.** Never `/CSR` and `/CSW` together. Data
ports tri-stated before `/CSR` falls. Blocks of at most 256 bytes, each
acknowledged, because the Nano has 2 KB of RAM; the serial receive buffer is
raised with a build flag so a block arrives whole before any of it is put on the
bus.

The serial rate caps throughput at 100 KB/s, less the per-block
acknowledgements, so all 64 KB of VRAM takes one to two seconds. Bursts from the
Nano's buffer still run faster than any 6502.

### The wire format

Both directions use one framing. Every integer is little-endian.

| Offset | Size | Field |
|--:|--:|---|
| 0 | 2 | `NB` (`$4E $42`) |
| 2 | 1 | type |
| 3 | 1 | sequence: the host's choice, echoed in the answer |
| 4 | 1 | payload length, 0 to 255 |
| 5 | length | payload |
| 5 + length | 2 | CRC-16/CCITT-FALSE of bytes 2 to the end of the payload |

A receiver hunts for `NB`, reads the header, and drops a packet whose CRC does
not match. The Nano answers a bad CRC with an ERROR. It answers each command
before it reads the next.

A block carries at most **240 bytes**, so the largest packet is 247. The limit
is the Arduino core's 256-byte receive ring, which has to hold a whole packet
before any of it goes on the bus. It cannot be raised: past 256 the core widens
its ring indices to `uint16_t` and serial stops working on this part altogether.
254-byte blocks make 262-byte packets, which work in isolation and desync under
sustained load.

**Types.** A command is `$01`–`$7E`. Its answer is the command with bit 7 set.
`$FF` is ERROR, and its payload is one byte:

| Code | Meaning |
|--:|---|
| 1 | the packet's CRC did not match |
| 2 | unknown command |
| 3 | wrong payload length for that command |
| 4 | the payload did not arrive within 200 ms |

### Commands

| Code | Name | Payload in | Payload out |
|--:|---|---|---|
| `$01` | PING | — | firmware(1), protocol(1) |
| `$02` | PROFILE | setup(1), width(1), hold(1), gap(1) | — |
| `$03` | WRITE | port(1), value(1) | — |
| `$04` | READ | port(1) | value(1) |
| `$05` | WRITE_BLOCK | port(1), bytes(1–240) | — |
| `$06` | READ_BLOCK | port(1), count(1) | bytes(count) |
| `$07` | RESET | microseconds(2) | — |
| `$08` | IDLE | — | — |
| `$09` | INT | — | level(1), edges(2), ticks(4) |
| `$0A` | INT_PERIOD | count(1), budget(1) | edges(1), first(4), last(4) |

`port` is `MODE1:MODE`, which is the CPU's `A1:A0` — the port decode in section
3. INT's `ticks` is Timer 1's count at the last falling edge, and Timer 1 runs
at F_CPU, so one tick is 62.5 ns.

`setup`, `width` and `hold` are loop counts on the Nano, three cycles each, and
`gap` is microseconds. What matters is the strobe they produce, which also
carries the loop's own test and the closing `out`:

| | `/CSW` low | data sampled after `/CSR` falls |
|---|--:|--:|
| width 0 | 4 cycles = 250 ns | 3 cycles = 187.5 ns |
| width n | 3n + 2 cycles | 3n + 3 cycles |

| Profile | setup | width | hold | gap | strobe |
|---|--:|--:|--:|--:|--:|
| `6502-1mhz` | 2 | 2 | 2 | 4 µs | 500 ns |
| `6502-2mhz` | 1 | 0 | 1 | 2 µs | 250 ns |
| `fastest` | 0 | 0 | 0 | 0 | 250 ns |

**250 ns is the floor** — a 2 MHz 6502's strobe width — which is why
`6502-2mhz` and `fastest` share a width and differ only in setup, hold and gap.

Everything on the strobe's critical path is force-inlined with its operands in
registers before the strobe falls. Left to itself the compiler puts a `call` and
an `lds` between the two `PORTC` writes, which makes the narrowest strobe 875 ns
— wider than a 1 MHz 6502's, so no margin sweep could find an edge.

`$0A` INT_PERIOD catches `count` falling edges of `/INT`, reading the status
port after each so the card can assert it again, and returns how many it caught
with Timer 1's count at the first and the last. Its payload in is count(1) and a
budget in hundredths of a second(1); out is edges(1), first(4), last(4). This
has to happen on the Nano: a host round trip is about 4 ms against a 16.68 ms
frame, so from there frames would be missed and the average would come out a
multiple of the period rather than the period.

The general script runner — wait, delay, compare, loop — is `$0B` onward and
arrives with Phase 13's timing work.

**The host side** is `tools/lib/nano.mjs` and `vdpctl bus`.

### The host talks through `serialport`, not `stty`

`tools/lib/link.mjs` opens the debug link as a tty and sets it raw with `stty`,
with no dependency. That works only because USB CDC's baud rate is nominal. The
Nano's is not, and **macOS `stty` cannot set 1,000,000 baud** — it needs the
`IOSSIOSPEED` ioctl, which Node cannot issue. Nor is there a rate that suits
both: `stty` takes 115200, 230400, 460800 and 921600, while a 16 MHz AVR divides
exactly to 1M, 500k, 250k and 200k. The two sets do not intersect. 460800 and
921600 are 8–13% off on a 16 MHz AVR, and 230400 is 3.5% off, outside what a
UART tolerates.

So `tools/` carries one dependency, `serialport`, exactly as PLAN.md section 5
anticipated. 115200 is the only `stty`-settable rate the Nano can hit, and at
nine times the transfer cost it is the fallback of last resort, not the plan.

**One wrinkle, measured on Node 26 with serialport 13 and this CH340 bridge:**
the binding's reader does not wake the event loop by itself. Bytes are delivered
only when the loop is woken for something else, so a reply can sit unseen until
the next timer fires. `Nano` keeps a 1 ms timer running while a request is in
flight, which brings round trips to about 4 ms. pyserial on the same port is
prompt, so this is the binding and not the driver. If it is ever worth dropping
the workaround, the experiment is to run the same round-trip measurement under
Node 22.


---

6. Video Capture
----------------

The PRO's video leaves by the FFC cable and never touches the bus harness:

```
PRO → FFC → HDMI dongle → HDMI capture card → Mac
```

`vdpctl grab` runs `ffmpeg -f avfoundation` and saves a PNG. The first run
triggers macOS's camera permission prompt for the calling app — grant it to the
terminal, not to Node. `vdpctl compare-capture` downsamples a grab to 320 × 240,
maps each pixel to the nearest palette entry, and reports the match rate against
a golden or a snapshot, with a mismatch image.

**Capture is never the pass/fail oracle.** The snapshot over the debug link is.
The dongle resamples the DAC's output on its way to HDMI and the card
compresses, so an exact match is not available. What capture proves is what a
snapshot cannot see: sync lock, DAC bit order, line doubling, and a picture that
stays stable under load. It is used in Phases 9, 10, 13 and 14.

### A capture against a golden

From Phase 10, `vdpctl inject --capture` grabs the picture each checkpoint
leaves on the monitor and scores it against that checkpoint's golden frame,
expanded through §11's palette. `tools/lib/screen.mjs` holds the method and the
tolerance; the short version is that the path is allowed a place, a black level
and a gain, and is then judged on the pixels far enough from a colour change to
have settled. What the path was measured to do, on this bench:

| | |
|---|---|
| Where the picture sits | −1 to 4 pixels across, 0 to 3 lines up, and it moves when the board is reset: the dongle locks sync where it likes |
| How far a colour change reaches | about 8 capture pixels, which is 4 card pixels. A change of brightness alone costs nothing down the screen: the path carries chroma at half resolution both ways |
| A settled pixel, after one gain and one black level per channel | within 8 levels of the golden, 99.85% of the time or better; worst seen 17 |
| A whole picture, edges and all | 2 to 31 levels of mean error, worst where the detail is two pixels wide everywhere |
| Two grabs of one picture | 0.24 levels apart |

### The bench cards

`bench/cards/` holds pictures this repository draws itself, for the things a
capture is good at: flat colour, and a lot of it (`tools/card.mjs`,
`tools/lib/cards.mjs`). Each is a trace and a golden of the oracle's shape, so
`vdpctl card <name>` injects it like any checkpoint and then measures what each
palette entry became, and PLAN.md's Phase 10 is what they were built for.

- **`palette`** — all 256 entries of §11 as swatches, with the family down the
  side and the step across the top. This is the picture PLAN.md's Still Open 3
  is judged on.
- **`dac`** — each channel's sixteen levels on its own, over a palette the card
  writes, and bands of one-pixel stripes. Each channel's ramp is measured with
  the other two dark, so a swapped channel or a reversed nibble is arithmetic
  rather than opinion, and the stripes show the ×2 across and the two VGA lines
  a card line is sent as.

---

7. Bring-Up Order
-----------------

Phase 9 runs entirely on the PRO's **stock TMS9918A firmware**. If something
fails here, it is the bench — not this repo's firmware, which has not been
flashed yet.

1. Save the stock flash (section 2).
2. Fit the `MDE1` pin.
3. Mount the PRO; wire ground first, then the four control lines, then the data
   bus, then `/INT`. Fit R9 and R10 before powering anything.
4. Plug in the HDMI dongle, the capture card and both USB cables. Confirm a
   picture before the Nano is involved at all.
5. Upload the harness. Confirm the serial rate: 1,000,000 baud, or record the
   500,000 fallback.
6. `vdpctl bus` — ping, status, and a walking-one readback through VRAM. If the
   bit order is wrong, this is where it shows. Run it at each of the three
   profiles with `--timing`.
7. `vdpctl text` loads the font and puts the test screen up.
8. `vdpctl compare-capture` grabs a frame and scores it.
9. `vdpctl irq-timing`, `vdpctl sweep`, `vdpctl soak`, `vdpctl reopen`.

Phase 10 then replaces the stock firmware, and the Nano has nothing to do with
it: everything goes over the debug link, and the picture over the capture card.
The board must be in BOOTSEL for the first flash of this repo's firmware and
never again.

10. `cmake --build --preset pro-debug`, then
    `PICOVDP_PRESET=pro-debug vdpctl flash` with the board in BOOTSEL.
    `PICOVDP_PRESET` also picks the ELF a fault record is symbolised against.
11. `vdpctl inject all --capture` — every checkpoint, digitally and on the
    monitor.
12. `vdpctl card dac` and `vdpctl card palette`.
13. `vdpctl scenes`, `vdpctl late`, `vdpctl fault`.

---

8. What Phase 9 Proved
----------------------

All six criteria passed on 2026-09-23. The measurements and what went wrong on
the way are in `docs/results/phase-09.md`; the summary is:

| Criterion | Result |
|---|---|
| The captured screen shows the text | 46,080/46,080 pixels, **100.00%** |
| 10⁶ random VRAM bytes, `6502-1mhz` and `fastest` | 1,015,808 bytes each, **0 wrong** |
| The `/INT` period | **16.6838 ms**, 0.003% from 59.94 Hz |
| Strobe and hold margins | no setting the harness can produce failed |
| The serial rate | **1,000,000 baud**; the fallback is not needed |
| 100 port opens and closes | 16384/16384 bytes unchanged |

Two limits are worth carrying forward. The margin sweep found no failure edge,
so the Phase 11 baseline is *the harness's floor* — a 250 ns `/CSW`, data
sampled 187.5 ns after `/CSR` falls, no setup, no hold, no gap — and not the
card's limit, which the Nano cannot reach.

And **MODE1 is untested.** The stock firmware decodes MODE alone and ignores
MODE1, so ports 2 and 3 behave as 0 and 1. The soldered `MDE1` pin stays
unproven until this repo's firmware decodes all four ports in Phase 11.

---

9. The Schematic
----------------

```
bench/hardware/
  picovdp-bench.kicad_pro
  picovdp-bench.kicad_sch
  picovdp-bench.pdf                  exported, for reading without KiCad
  sym-lib-table
  libraries/picovdp-bench.kicad_sym  the PICO9918 PRO symbol
```

Drawn with KiCad 10. It is a **wiring reference, not a board**: there is no PCB,
no footprints are assigned, and nothing here is meant to be fabricated. Its
value is that it is checkable — `kicad-cli sch erc` passes with zero errors and
zero warnings, and its netlist is the table in section 3.

The `PICO9918 PRO` symbol is the 40-pin DIP TMS9918A pinout, carried over from
the `Pico9918` symbol in the 6502-ACE project's `6502 Parts` library so the
bench and the AC6502 card agree pin for pin. Everything else comes from KiCad's
standard libraries.

To re-export the PDF:

```
kicad-cli sch export pdf -o bench/hardware/picovdp-bench.pdf \
    bench/hardware/picovdp-bench.kicad_sch
```
