6502-PICOVDP — Firmware Plan
============================

How the video card specified in [SPEC.md](SPEC.md) gets built, proven and
delivered as firmware for the PICO9918 PRO v2.0.

**Target:** SPEC.md draft 0.5, in full, on a PICO9918 PRO v2.0 (RP2354A), fitted
to an AC6502.

**Definition of done:** the unmodified BIOS boots to `OK` on an AC6502 with the
PRO running this firmware; every golden checkpoint the emulator holds reproduces
byte for byte on the PRO; and the per-line budget, interrupt timing and status
freshness are measured on the PRO and written back into SPEC.md.

**Status:** Phases 0–11 done ([results](docs/results/)). Phase 1 measured
the line at 2.5–4× §18's estimates, and settled SPEC.md draft 0.4 from it: the
sprites are built on core 0 (section 3), the clock is 352 MHz, `SPRLIMIT` resets
to 16, and a late line is specified. Phase 2 exported the oracle: the fixtures'
traces replay every golden exactly with no CPU, all fifteen checkpoints are
static, and `Video.test.ts` runs against the core. Phase 3 put the bus side in
C — ports, registers, VRAM with its journal and render copy, the palette — and
it matches `Video.ts` over 10⁷ fuzzed operations. Phase 4 added the raster's
line numbering, the sixteen status registers and the interrupts, and they match
`Video.ts` in every read and in `/INT` over 10⁷ more. Phase 5 added the tile
engine at 1bpp and the legacy submode: the `bios` goldens reproduce on the
host, and every frame matches `Video.ts` over 10⁷ more. Phase 6 added the
sprites, evaluated at the latch and drawn divided between the cores: the
`wizardslab` goldens reproduce on the host, every split column builds the same
line, and reads, `/INT` and frames match `Video.ts` over 10⁷ more with sprites
on. Phase 7 added 2, 4 and 8bpp, the attribute byte, layer 1's contest with
layer 0 and the 4bpp table: the whole oracle — all fifteen checkpoints —
reproduces on the host through both the Node adapter and the pure-C
`host/replay`, all 342 tests of `Video.test.ts` pass against the core, and
10⁵ frames of random layer scenes match `Video.ts`. Phase 8 put all of it but
the bus on a Pico 2, after rewriting the renderer a word at a time and building
each row in two halves, one a core: all fifteen checkpoints reproduce on the
board by injection, Phase 1's worst cases run ten minutes with no late line while
snapshots stream, a late line on purpose repeats its predecessor with status
untouched, and faults are recovered over USB. Under Phase 1's bus stand-in,
`SPRLIMIT` 16 keeps 16–24% of the line, not 25%. Phase 9 built the bench and
proved it on the PRO's stock firmware: the wiring is right, a captured screen
matches a reference rendered from VRAM read back over the bus in all 46,080
pixels, 10⁶ random VRAM bytes round-trip with no error at either profile, and
the `/INT` period is 16.6838 ms — the PRO is a 59.94 Hz part. Phase 10 put this
firmware on the PRO: all eighteen checkpoints reproduce there by injection, each
one's picture is captured off the monitor and scored against its golden, the
DAC card measures every channel's sixteen levels with no cross-talk and the
palette card carries all 256 of §11's entries with every ramp still a ramp,
and the worst-case scenes, the late line and the faults all behave as they did
on the Pico 2 — whose budget the PRO matches to 0.4% of a line. Phase 11 put
the bus on the pins: four ports through a read program that stages all four
answers in one word, and 10⁷ random accesses at each of the Nano's three
profiles — every command form, both pairs — with no read wrong against
`Video.ts` and the whole card exact at every check; reads 4 µs apart back to
back always right; RST leaving §15's state 50 times in 50; no FIFO overrun.
Reads 2 µs apart are stale about once in 170, behind core 1's latch interrupt,
which Phase 13 inherits. **Phase 12, the traces through the bus, is next.**

---

Contents
--------

1. [Scope](#1-scope)
2. [Ground Rules](#2-ground-rules)
3. [Architecture](#3-architecture)
4. [The Oracle and How It Is Used](#4-the-oracle-and-how-it-is-used)
5. [The Bench](#5-the-bench)
6. [Phases](#6-phases)
7. [Risk Register](#7-risk-register)
8. [What This Plan Owes SPEC.md](#8-what-this-plan-owes-specmd)

[Appendix A — What Comes From pico9918](#appendix-a--what-comes-from-pico9918)

---

1. Scope
--------

### In scope

- Firmware for the PICO9918 PRO v2.0 implementing SPEC.md draft 0.5: four ports,
  128 registers, 64 KB VRAM, the tile engine at all four depths, both layers,
  sprites, the palette, compositing, scrolling, status and interrupts, reset.
- A portable C core holding all of that behaviour, built for the RP2350 and for
  macOS, with a host test suite.
- Additions to `6502-EMULATOR` that export its oracle: a
  port observer, port traces of the golden fixtures, a CPU-less trace replay,
  and a Jest configuration that runs its video tests against this repo's core.
- A bench that drives the PRO without an AC6502: an Arduino Nano bus harness, a
  USB debug link in the firmware, video capture, and the host tools that tie
  them together.
- Debug and release builds; acceptance in an AC6502.

### Not in scope

- **PICO9918 v1.x (RP2040).** SPEC §2.
- **SCART output.** SPEC §2 leaves 480i/576i timing unspecified. VGA only, which
  also serves the HDMI dongle.
- **SWD debugging as a dependency.** The PRO's `DBG1` SWD connector is unplaced on
  stock boards and sits on the underside, facing the socket. Section 3's debug
  link replaces it, and section 3 also says where the pads are if it is ever
  needed.
- **pico9918's configurator, flash configuration, splash, GPU coprocessor and
  on-screen diagnostics.** See Appendix A.
- **Kernal changes** (SPEC §17) — they belong to `6502-BIOS` and follow this plan.

In this plan, **§** always means a section of SPEC.md; the plan's own sections
are written out as "section N".
- **§19's future space** — blitter, bitmap mode, display lists.

### Sequencing

Everything SPEC.md specifies except the bus pins, the video DAC and the raster
timing can be proven before the PRO exists: on the host against the emulator,
and on a Pico 2's RP2350A — the same silicon as the PRO's RP2354A, which is an
RP2350A with 2 MB of flash in the package — by feeding it bus traffic over USB. When the PRO arrives, the hardware phases are left with
only what needs the PRO: the pins, the picture and the clock.

---

2. Ground Rules
---------------

1. **SPEC.md is the specification. `6502-EMULATOR`'s `Video.ts` is the reference
   implementation.** Where firmware and emulator disagree, one of them is wrong,
   and it is decided in the spec first (SPEC §18). Do not encode a behaviour that
   is not written down.
2. **The spec has three copies, kept in step.** `SPEC.md` here, `docs/VDP-SPEC.md`
   in `6502-EMULATOR`, and the HTML rendering, `docs/SPEC.html` here, which is
   published from that file to the same artifact URL each time. A change to one is
   a change to all three, made in the same sitting — the emulator's `CLAUDE.md`
   holds that repo to the same rule. `SPEC.md` is canonical: CTest
   `spec_in_step` (`tools/check-spec.mjs`) fails when the emulator's copy
   differs from it or the HTML names another draft.
3. **Emulator changes land on `main`.** `v3-vdp` was merged for 3.0.0 and
   receives nothing more; the emulator's own `CLAUDE.md` governs its commits,
   including: goldens are not edited to pass.
4. **Goldens belong to the emulator.** This repo holds a pinned copy under
   `tests/oracle/`, written only by `tools/sync-oracle.mjs`, never by hand. A
   golden that needs to move is re-captured in `6502-EMULATOR`, in a commit of its
   own, and then re-synced here in a commit of its own.
5. **The core stays portable.** Nothing under `core/` includes a pico-sdk header
   or touches hardware. What the host suite proves about `core/` is then proven
   about the code that ships.
6. **Every phase ends green.** The host suite passes at every phase boundary — and,
   from Phase 2 on, the Jest run against the core and the fuzzer; from Phase 8 on,
   the phase's run on silicon. Results go in `docs/results/phase-NN.md`. "Done when" is
   checked, not assumed.
7. **Measured beats estimated.** SPEC §18's figures are estimates until a phase
   measures them. Measurements go into SPEC.md (rule 2 applies).
8. **Debug features don't change behaviour.** Snapshot, injection and statistics
   compile out of release builds, and a release build must reproduce the debug
   build's results.
9. **Spec section numbers in code comments.** `// §8` beats a paraphrase that
   drifts. The same convention as `Video.ts`.
10. **Every phase ends in a commit, made without being asked.** When a phase's
    "Done when" checks pass and `docs/results/phase-NN.md` records them, commit
    that phase's work to `main` in this repo, titled `Phase N: <phase title>`.
    Its body lists what was done and anything that differs from this plan. Stage
    files by name, never `build/` or other generated output. Emulator work from
    the same phase is committed in `6502-EMULATOR` on `main`, under that repo's
    rules. Oracle re-syncs keep their own commits (rule 4). A phase that is not
    green gets no phase commit: report what failed instead. Never push; pushing
    stays the owner's call.

---

3. Architecture
---------------

### Repository layout

```
  SPEC.md  PLAN.md  README.md  LICENSE  THIRD_PARTY.md
  picovdp-default.gpl  .pal  -reference.png      (existing palette files)
  CMakeLists.txt  CMakePresets.json               host, host-asan, pico2, pro-debug, pro-release
  core/            portable C11 — all of SPEC.md's behaviour
  firmware/        RP2350 only — main, bus PIO, VGA driver, debug link, fault handling
    vga/           from pico9918 (MIT), VGA-only
    boards/        pico9918pro.h (from pico9918), pico2 configuration
  host/
    node/          N-API adapter: the core behind Video.ts's interface
    replay/        vdp-replay — pure-C trace replay CLI
  spike/           Phase 1 timing spike (disposable)
  bench/nano/      Arduino Nano bus harness (PlatformIO, nanoatmega328new)
  bench/hardware/  the harness as a KiCad schematic, with its own symbol library
  tools/           Node ESM, no build step: vdpctl, fuzz, sync-oracle, lib/
  tests/
    unit/          C unit tests, by SPEC section
    oracle/        pinned goldens + traces + manifest (tools/sync-oracle.mjs only)
    bench/         port-level conformance scripts for the Nano
  docs/            TRACE.md, DEBUGLINK.md, BENCH.md, results/
  external/pico-sdk   submodule, pinned to 2.1.1 (with lib/tinyusb initialised)
```

pico-sdk stays at **2.1.1**, which is what pico9918's CI builds; its
`BUILDING.md` records linker and memory failures under 2.2.0. The RP2040
fast-boot patch pico9918 carries does not apply to the RP2350.

### The core

One state structure, no globals, so the host can run many instances. Its
interface is small and is the same on both platforms:

```c
void    vdp_init(vdp_t *v, uint8_t version);                // power-on reset; STAT5 = version (Phase 3)
void    vdp_reset(vdp_t *v, bool power_on);                 // §15; RST leaves the raster running
uint8_t vdp_read(vdp_t *v, unsigned port);                  // port = A1:A0, §4
void    vdp_write(vdp_t *v, unsigned port, uint8_t value);  // §4
void    vdp_line_start(vdp_t *v, uint16_t screen_line);     // a screen line begins, §3: latch, catch up, publish (host)
void    vdp_latch(vdp_t *v, uint16_t screen_line, uint32_t tag); // its bus side, as the line begins
bool    vdp_catch_up(vdp_t *v);                             // its render side, when the renderer is ready
void    vdp_set_hblank(vdp_t *v, bool hblank);              // STAT3 b1, from the platform, §6
int     vdp_split_choose(const vdp_t *v);                   // the column the cores divide this row at
void    vdp_build_half(const vdp_t *v, vdp_half_t *h, int x0, int x1); // columns [x0, x1): layers, sprites; no status
void    vdp_expand_half(const vdp_t *v, const vdp_half_t *h, uint16_t *rgb);   // its columns, 12-bit 0x0BGR, ×2
void    vdp_copy_half(const vdp_t *v, const vdp_half_t *h, uint8_t *indices);  // its columns as indices
void    vdp_publish(vdp_t *v, const vdp_half_t *a, const vdp_half_t *b); // the row's overflow and collisions
void    vdp_build_line(vdp_t *v, uint8_t *indices);         // both halves on one thread (host)
void    vdp_expand_line(const vdp_t *v, const uint8_t *indices, uint16_t *rgb); // 12-bit 0x0BGR, ×2
bool    vdp_int_asserted(const vdp_t *v);                   // §14
```

Inspecting the card without disturbing it — registers, VRAM, ports, the palette,
the mode, statistics, snapshot save and restore — is `core/vdp_debug.h`, a
library of its own that the adapter, the unit tests and the debug link link and
a release image need not.

The core is split into a **bus side** — ports, register file, VRAM, status and
interrupt latches, which `vdp_read`/`vdp_write` touch — and a **render side**,
which only the build functions touch. The latch is where the two meet:
`vdp_latch` on the bus side, `vdp_catch_up` on the render side, and
`vdp_publish` bringing what a row found back to status. `vdp_line_start` is all
three at once.

**Why the split exists.** SPEC §3 builds display line N from the registers, VRAM
and palette *as they stand when line N − 1 begins*. On the RP2350 the build takes
a large part of that line, and bus accesses arrive as interrupts on the same core
throughout it. They cannot be deferred: a second read 4 µs after the first needs
its prefetched byte staged already (§4). So the renderer cannot read live state.
Instead:

- **VRAM is held twice.** Bus writes land in the bus copy at once, so reads on
  either port see them at once, and are appended to a journal, a ring of 1024.
  The catch-up drains it, to where the latch found it, into the render copy. The
  build reads only the render copy, which does not change under it. A journal
  that overflows — no 6502 can do it at 1024 entries, but the Nano's fastest
  profile might — falls back to copying dirty 1 KB pages, and counts that it did.
  128 KB of VRAM is comfortable in 520 KB of SRAM.
- **The latch comes in two halves** (Phase 8). `vdp_latch` runs in core 1's
  interrupt as the line begins: the line's events (§14), and a record of the
  register file and the journal's end. `vdp_catch_up` runs in the renderer when
  it is ready for the line, which after a late line is later (§18), and brings
  the render side to exactly that record. Records wait in a ring of 8; a latch
  that finds it full is merged into the newest, and the line it held is never
  built.
- **Registers are snapshotted** by the latch, with the derived per-line state
  (geometry, bases, scroll, palette base) recomputed from them.
- **The palette cache** — 256 entries of paired 12-bit pixels, `x × $10001`, the
  way pico9918 doubles horizontally — is updated from journal entries that fall
  in the palette window as it drains, and re-read whole when `PALBASE` changes.
  That gives SPEC §11's "the next line built uses it" exactly.
- **Sprite evaluation for the line about to be built runs in the catch-up.**
  Its overflow, and the collisions the build finds, reach status through
  `vdp_publish`, which core 1 calls with its interrupts held off once the row is
  built. Through `vdp_line_start` and `vdp_build_line` on the host that is at the
  latch and in the build, as the emulator does; on the RP2350 it is within the
  line, and its lag is measured in Phase 13.
- **Each row is built in two halves, one a core** (SPEC §18, Phase 8).
  `vdp_split_choose` picks a column on an 8-pixel boundary, balancing both
  layers, the expansion and each sprite at its centre. Core 0 builds picture
  columns left of it — both layers and the sprites — into a half of its own and
  expands them into the row's buffer; core 1 does the same for the rest. Nothing
  in a column depends on another's pixels, so the halves are exact: each has its
  own claim bitmap for priority among sprites and collision. Neither writes
  status; core 1 publishes both. The firmware moves the column 8 pixels a row
  toward the core that finished first. The host runs the same functions on one
  thread, and the reference check builds each line at many splits.
- **The screen line is an input.** The raster belongs to the platform — the VGA
  driver on the RP2350, the adapter's model of `Video.ts` on the host — and
  `vdp_line_start` is told which of the 262 screen lines is beginning. The core
  derives the display line from it with the geometry in effect, which is all
  §3's 24-line move across a change of picture height amounts to.
- **`STAT3` b1 (horizontal blanking) is an input**, supplied by the platform
  through `vdp_set_hblank`: the VGA timing on the RP2350, a model of `Video.ts`'s
  timing on the host. §6 calls the bit advisory.

Hot functions carry a `VDP_HOT` attribute that maps to `__not_in_flash_func`
on the RP2350 and to nothing on the host. The whole image is `copy_to_ram`, as
pico9918's is.

### Firmware shape

The PICO9918 structure carries over (SPEC §18), with priorities made explicit —
pico9918 sets none:

| Core | Work | Priority |
|---|---|---|
| 0 | VGA: pico9918's driver, whose sync program now raises a PIO interrupt at the start of **every one of the 262 screen lines**. Its handler rings core 1's doorbell and starts the row's RGB buffer: the latest finished row, which is a late row's predecessor. The sync and RGB DMA interrupt beside it | highest on core 0 (`$00`) |
| 0 | Core 0's half of each row, posted by core 1 through the inter-core FIFO: `vdp_build_half`, `vdp_expand_half` | `$40` |
| 0 | USB, the SDK's timer, the watchdog's feeder | default and below |
| 0 | The debug link (debug builds) | thread |
| 1 | The latch, from core 0's doorbell: `vdp_latch` for every line begun. From Phase 11, the bus PIO interrupts at the same priority, so they never preempt one another | highest on core 1 (`$00`) |
| 1 | The renderer: `vdp_catch_up`, `vdp_split_choose`, post to core 0, `vdp_build_half` and `vdp_expand_half` for the rest, wait for core 0, `vdp_publish` with interrupts held off — into a buffer no line start can be sending. A row not finished when its line starts is late (SPEC §18): the line sends the last finished row again, and the build finishes for its status | thread |

Phase 1 put the sprites in core 0's thread and the debug link in its lowest
interrupt. Phase 8 swapped them: a USB write blocks, and a sprite build that USB
could preempt is a late line waiting to happen; in an interrupt above USB it
cannot be.

The clock is 352 MHz (PLL 1056 MHz ÷ 3, VREG 1.30 V), pico9918's VGA preset 2.
A display line is 2 × 1,598 PIO ticks at a divider of 7: **22,372 cycles**.

The bus interface follows SPEC §2 and §18. `tmsWrite` is kept as it is: MODE1
already arrives in bit 31 of its FIFO word. `tmsRead` is rewritten. The CPU stages
**four bytes** in one 32-bit word — data A, status A, data B, status B — and the
program samples MODE and MODE1 with `in pins, 2` and skips to the right byte. Pin
directions come from a constant rather than from the word, which frees the 8 bits
pico9918 spent on them. The word it pushes back tells the handler which port was
read and whether it was data or status. The handler advances that port's prefetch
or acknowledges its status, and restages. The latch restages whenever status
changes. The final program is Phase 11's to write; these are its constraints.

pico9918's start-up has DMA interrupts running before core 1 takes its handshake
off the FIFO, so the pop can consume a line request instead. This firmware
starts core 1's latch only after the handshake.

### Builds

| Preset | Runs on | Debug link | Snapshot, injection, stats | Watchdog, safe mode |
|---|---|:--:|:--:|:--:|
| `host`, `host-asan` | macOS | — | — | — |
| `pico2` | Pico 2 (VGA pins driven, no bus) | ✓ | ✓ | ✓ |
| `pro-debug` | PRO | ✓ | ✓ | ✓ |
| `pro-release` | PRO | — | — | watchdog only |

The PRO's MCU is an **RP2354A**: an RP2350A die with 2 MB of flash in the
package, where the Pico 2 has 4 MB beside it. Nothing in the build depends on the
difference. Every preset targets `rp2350-arm-s`; `pico9918pro.h` declares the 2 MB;
the image is `copy_to_ram`, about 200 KB of flash, and nothing touches flash after
boot.

### The debug link

With no SWD, everything goes through the PRO's USB-C port, which is on the side
that faces up and also powers the board. It is specified in `docs/DEBUGLINK.md`
(Phase 8).

- **Flashing:** `picotool load -x -f`. The debug build includes `pico_stdio_usb`,
  whose reset interface lets picotool reboot a running board into BOOTSEL and
  flash it. The BOOT button is only needed the very first time.
- **Transport:** framed binary packets over USB CDC, CRLF translation off; log
  text travels as its own packet type. Specified in `docs/DEBUGLINK.md`.
- **Commands:**

  | Command | Returns or does |
  |---|---|
  | `INFO` | build id, clock, uptime, reset reason, the last fault record |
  | `STATS` | build cycles per line (DWT cycle counter): max, 99.9th percentile, mean. Also late lines, journal overflows, bus FIFO overruns |
  | `SNAPSHOT` | a complete frame — the next one, or the one an injection marks — as 240 rows of indices exactly as sent to VGA, with the register file, both ports' state, and status and latch internals as they stood when its last row was built |
  | `VRAM` | the bus copy of VRAM, 64 KB |
  | `INJECT` | a stream of port operations, each tagged with the (frame, screen line) it fell in and the read and `/INT` it expects, and the frame to capture; core 1 takes each latch in its thread, in a trace replay's order, applies each line's operations after the row it starts is built, as if from the bus, and reports what differs |
  | `RESET` | SPEC §15, as the RST pin does |
  | `REBOOT` | into the application or into BOOTSEL |
  | `FAULT` | debug builds only: raise a deliberate fault to test recovery |
  | `SCENE`, `LOAD`, `SCENE LOG`, `PROFILE` | Phase 8: a worst-case scene set up on the card; a bus stand-in and a handicap on the build; what the scene's program read each frame; one row's stages timed with interrupts off |

- **Crashes and hangs:** a HardFault on either core, or a panic, writes a record —
  core, PC, LR, xPSR, the fault status registers, SP, 32 stack words — to a
  section that survives reset. It then reboots through the watchdog. The watchdog
  (250 ms) is fed by core 0 only while core 1's heartbeat advances. After any
  abnormal reset the firmware comes up in **safe mode**: USB up, bus and renderer
  off, backdrop a fixed colour the capture can see. It waits for `INFO` and a new
  image. Addresses are symbolised with `arm-none-eabi-addr2line` against the
  build's ELF.
- **The one manual recovery:** an image that dies before USB enumerates. Hold BOOT
  and replug USB. The debug build brings USB up before anything but clocks to
  make that rare.
- **If a debugger is ever needed:** `DBG1`'s three pads are on the underside,
  beneath the BOOT button, next to the CD0–CD3 pins. Pad 1 is SWCLK, 2 is GND and
  3 is SWDIO (schematic sheet 2; confirm GND with a meter). Three wires to the
  Pico 2 flashed with Raspberry Pi's `debugprobe` firmware would do. This is a
  fallback, not a step of any phase.

---

4. The Oracle and How It Is Used
--------------------------------

### What the emulator holds

`6502-EMULATOR/src/tests/goldens/` holds five fixtures and eighteen checkpoints.
Since emulator 3.1.0 all five boot `BIOS2.bin`, BIOS 2.0, at 1 MHz; `vdp-font`
never calls `KernalInit`. The oracle has no BIOS 1.6 boot on this card (the
emulator's `tms9918a/bios` golden is the other card's).

| Fixture | Program | Checkpoints |
|---|---|---|
| `bios` | BIOS 2.0 alone, with typed input: Text mode, the logo and header, a scroll through `L0SCRY` | `ok`, `screenful`, `scroll` |
| `wizardslab` | `WizardsLab.crt`, Graphics I, sprites, vblank IRQ | frame 60, 180, 300, 600 |
| `vdp-modes` | `VdpModes.crt`: the four geometries at 1, 2, 4, 8bpp | `text`, `compact`, `graphics`, `full` |
| `vdp-layers` | `VdpLayers.crt`: two scrolling 4bpp layers in Full mode, sprites at levels 1–6 | `parallax`, `scroll-bit8-l1`, `occluded`, `scroll-bit8-l0` |
| `vdp-font` | `VdpFont.crt`, which never calls `KernalInit`: the built-in font at reset, loaded by `FONT` and relocated (draft 0.5, §7) | `reset`, `loaded`, `relocated` |

Each checkpoint has `*.idx.bin`, the frame as 76,800 palette indices, row-major;
`*.vram.bin`, the full 64 KB; `*.json`, with registers, mode, `STAT0`, a VRAM
hash and the text grid; and `*.png`. **The index frame is the oracle**: it is
compared exactly and is immune to palette changes.

`src/tests/IO/Video.test.ts` holds **364 tests** (342 until draft 0.5's font) that drive a bare `Video` through
its ports with no CPU. They cover every section of the spec, raster-split timing
included.

### Traces

A trace is a fixture's VDP port traffic, recorded from the emulator and specified
in [`docs/TRACE.md`](docs/TRACE.md). It is gzip-compressed text,
`<fixture>.vdpt.gz`, kept beside the fixture's goldens in the emulator. It has a
header naming the fixture and emulator commit, then one event a line, each with
its tick delta:

- a cold reset
- every line start, with its screen and display line
- each write and read, with port and value (for a read, the value returned)
- changes of `/INT`
- each checkpoint, with its cycle count, golden frame, **settle point**, window
  and **class**

A checkpoint's golden frame is the last one presented before it, and its VRAM and
registers are as they stand at the checkpoint itself. These are not the same
moment: `vdp-layers`, for one, writes the *next* frame's scroll registers in
vertical blank, after its frame is finished and before the checkpoint is taken.
The **settle point** is the number of operations before the golden frame's first
row was latched. A checkpoint is **static** if its frame is a function of the
state at the settle point alone: replayed that far, with nothing more applied,
the card presents the golden frame. Phase 2 found all fifteen static. It decided
this by running a frozen replay, not by counting operations between the frame's
first and last row latches: twelve checkpoints have status polls in that window,
and a status poll changes no picture.

### Four executors, one oracle

| Executor | Drives | Timing | Checks |
|---|---|---|---|
| **Reference replay** — `6502-EMULATOR/scripts/replay-trace.mjs` | `Video.ts`, no CPU | exact: ticks to each operation | reproduces the goldens; defines expected reads |
| **Host** — `host/node` adapter and `host/replay` CLI | `core/` on macOS | exact | goldens, all 364 Jest tests, fuzz against `Video.ts` |
| **Injection** — `vdpctl inject` | firmware on a Pico 2 or PRO, via USB | exact: applied at each operation's (frame, line), with a snapshot marker on each golden frame | goldens on silicon, static and dynamic alike |
| **Bus** — `vdpctl replay` | firmware on the PRO, via the Nano and real pins | untimed | static checkpoints exactly; VRAM reads; timing-independent status |

**Running Video.test.ts against C.** Rather than port 364 tests by hand, the
emulator gains `jest.picovdp.cjs`. It maps `src/core/IO/Video` to the N-API
adapter in `host/node` and runs the test file unchanged. The adapter presents
the surface the tests use, and reproduces `Video.ts`'s per-cycle accumulator
(`f / 60 / 262`) in JavaScript to decide when to call `vdp_line_start`. Surface
used: construction, `read`, `write`, `tick`, `getVramByte`/`setVramByte`,
`frameIndices`, `buffer`, `getStatus`, `peekStatus`, `getDisplayLine`,
`portState`, `paletteEntry`, `getRegister`/`setRegister`, `getMode`,
`isDisplayEnabled`, `textGrid`, `readVRAM`/`writeVRAM`, `vramSize`, and in three
tests `serialize`/`deserialize` — snapshots, which the core carries: the adapter
writes and reads `Video.ts`'s own format through `vdp_debug.h` (Phase 3). Any
test that reaches into the TypeScript class's internals is listed in the config
as skipped, by name, with the reason — never skipped silently. Phases 2 and 3
found none. `PICOVDP_ADDON` names the adapter module,
`host/node/Video.cjs`, which loads the compiled addon itself.

**Fuzzing.** `tools/fuzz.mjs` loads the emulator's compiled `Video` and the
adapter into one process. It feeds both the same seeded stream of port operations
and line starts, biased toward meaningful values, and compares every read, `/INT`
after every operation, and frames at intervals. A divergence prints the seed and
a minimised trace, which becomes a unit test.

**Known, deliberate differences** the comparisons must allow:

- `STAT5` — the emulator reports the spec revision (`$05`), the firmware its own
  version. The host adapter is configured to report `$05`; the bench excludes it.
- Frame numbers — the emulator's 262 equal lines at exactly 60 Hz do not match
  the PRO's 59.94 Hz raster (SPEC §18). Injection addresses frames and lines by
  count, not time, so it is unaffected. The bus replay is untimed.
- `STAT3` b1 — advisory (§6). It is not compared on silicon.
- Power-on VRAM — §15 leaves it undefined. The firmware zeroes it at power-on, as
  the emulator's cold start does, so VRAM goldens compare exactly. RST does not
  zero it.

### What only silicon proves

The host proves behaviour. It cannot prove that:

- the renderer fits its line on an M33 at 352 MHz
- the bus PIO sees every access at 6502 speed
- the latch and staged status work with interrupts on the same core
- the palette reaches the 12-bit DAC in the right bit order
- line doubling and sync are clean
- USB traffic leaves the raster alone

Phases 1, 8 and 10–13 exist for those.

---

5. The Bench
------------

Specified in full in `docs/BENCH.md`, and drawn in
`bench/hardware/picovdp-bench.kicad_sch` — a wiring reference, not a board to
fabricate. The PRO sits on a breadboard or a 40-pin socket, driven by an Arduino
Nano on the same breadboard. Its video goes to the Mac through the dongle and the
capture card, and its USB-C goes to the Mac for power and the debug link.

The Nano (ATmega328P) was chosen over the Mega 2560 for a cleaner breadboard.
It is the same 16 MHz AVR, so strobe timing and `/INT` capture resolution are
unchanged. What it gives up is RAM (2 KB against 8 KB), a free 8-bit port for
data, and spare input-capture timers. Each is accounted for below. The Mega stays
on hand as the fallback (section 7, risk 12).

### Parts

| Part | Status | Used from |
|---|---|---|
| Raspberry Pi Pico 2 | on hand | Phase 0 |
| PICO9918 PRO v2.0, HDMI dongle, FFC cable | on hand | Phase 9 |
| Arduino Nano (ATmega328P, 5 V, 16 MHz) | on hand | Phase 9 |
| Arduino Mega 2560 | on hand, fallback | — |
| HDMI capture card | on hand | Phase 9 |
| One 0.1″ male header pin for `MDE1` | needed | Phase 9 |
| 8 × 220 Ω resistors, 2 × 10 kΩ resistors, breadboard or DIP-40 socket, jumpers | needed | Phase 9 |
| 74HCT-family gate, for tapping frame sync into the Nano | optional | Phase 13 |

### Preparing the PRO

The `MDE1` pad (TMS9918 pin 11 position) has no pin fitted: MODE1 is not a
TMS9918 signal. Solder one header pin there. Port B (`$9C02`/`$9C03`) is
unreachable without it, on the bench and in the AC6502 alike. No other
modification is needed.

### Wiring

The table below is the schematic's netlist in words; the two are kept in step.

TI numbers the TMS9918 data bus backwards: **CD0 is the most significant bit and
CD7 the least**. pico9918 samples GPIO 14 (CD7) as bit 0. Get this wrong and
every byte arrives bit-reversed.

The Nano's `A0`–`A5` are header pins on its analog side, used here as digital
I/O. They have nothing to do with the CPU address lines that MODE and MODE1 stand
for.

| PRO label | TMS9918 pin | Signal | Nano pin | AVR port bit |
|---|:--:|---|:--:|:--:|
| CD7 | 17 | data bit 0 (LSB) | D2 | PD2, via 220 Ω |
| CD6 | 18 | data bit 1 | D3 | PD3, via 220 Ω |
| CD5 | 19 | data bit 2 | D4 | PD4, via 220 Ω |
| CD4 | 20 | data bit 3 | D5 | PD5, via 220 Ω |
| CD3 | 21 | data bit 4 | D6 | PD6, via 220 Ω |
| CD2 | 22 | data bit 5 | D7 | PD7, via 220 Ω |
| CD1 | 23 | data bit 6 | D9 | PB1, via 220 Ω |
| CD0 | 24 | data bit 7 (MSB) | D10 | PB2, via 220 Ω |
| CSW | 14 | `/CSW` | A0 | PC0, 10 k pull-up to 5 V |
| CSR | 15 | `/CSR` | A1 | PC1, 10 k pull-up to 5 V |
| MDE | 13 | MODE = CPU A0 | A2 | PC2 |
| MDE1 | 11 (fitted pin) | MODE1 = CPU A1 | A3 | PC3 |
| RST | 34 | `/RESET` | A4 | PC4 |
| INT | 16 | `/INT` (10 k pull-up on the PRO) | D8 | PB0 = ICP1 |
| — | — | frame sync, via the 74HCT gate (optional, Phase 13) | A5 | PC5 = PCINT13 |
| GND | 12 | ground | GND | |
| +5V | 33 | **leave unconnected** — the PRO runs from its USB-C | | |

`D0` and `D1` carry the USB serial link, and `PB6`/`PB7` and `PC6` hold the
crystal and reset, so the 328P has no free 8-bit port. Data is split: six bits on
PORTD, two on PORTB. Control stays on PORTC alone, so each strobe edge is still a
single port write:

- **On a write,** both data ports are set before `/CSW` falls.
- **On a read,** both are sampled while `/CSR` is low, one instruction (62.5 ns)
  apart, and the PRO holds the bus throughout.

Timer 1's input capture on `D8` timestamps `/INT` edges to 62.5 ns. That is the
328P's only input-capture pin. The optional frame-sync tap is timestamped
instead, from Timer 1's count inside its pin-change interrupt. That is good to a
few µs, against a 63.6 µs line. `D11`–`D13` stay free; `D13`'s on-board LED is
the harness's status light.

**Where that tap comes from is now open.** It was to be the VGA dongle's VSYNC
pin. The HDMI dongle has no analogue sync to clip onto, so if Phase 13 wants a
raster reference independent of `/INT`, this repo's firmware must bring one out
on a spare PRO GPIO. The tap is optional either way: Phase 13's latch and
interrupt measurements are all relative to `/INT`, which the input capture
already timestamps. Decided in Phase 13, not before.

The socket side of the PRO is 5 V logic, so the Nano needs no level shifting: a
74HC245 drives out and a 5 V-tolerant 74LVC245 receives. The 220 Ω resistors
protect against a harness bug driving the data lines while the PRO does. The
10 k pull-ups hold both strobes high while the Nano's pins float in reset. The
host opening the serial port pulses DTR, which resets the Nano, so without the
pull-ups the PRO would see stray accesses.

### The Nano harness (`bench/nano`)

A PlatformIO project that speaks a framed protocol at 1,000,000 baud. Its
environments are `nanoatmega328new`, for the current bootloader, and
`nanoatmega328`, for clones with the old one. The 328P's 16 MHz clock hits that
rate exactly. The Nano's USB bridge (FT232RL on genuine boards, CH340 on most
clones) and its macOS driver must also carry it. Phase 9 confirms this; the
fallback is 500,000 baud, which is also exact. The harness is a small bus-script
runner, not a byte relay, because anything timing-critical has to happen on the
Nano: USB serial latency is milliseconds.

- **Primitives:** write or read a port, block write or read, pulse `/RESET`, and
  timestamp `/INT` edges.
- **Timing profiles:** `6502-1mhz` (≈500 ns strobes, accesses ≥ 4 µs apart),
  `6502-2mhz` (≈250 ns, ≥ 2 µs) and `fastest`. Strobe width, data setup and hold
  can also be set directly, for margin sweeps.
- **Scripts:** executed locally with deterministic timing — wait for `/INT`
  (with timeout), delay µs, write, read and compare, record timestamp, loop.
  Scanline-interrupt tests run this way, reacting within microseconds. A script
  is held in RAM, so it is capped at 256 bytes. That is ample for Phase 13's
  loops, which are a handful of operations.
- **Invariants it enforces:** never `/CSR` and `/CSW` together; data ports
  tri-stated before `/CSR` falls; blocks of at most 256 bytes, acknowledged,
  because the Nano has 2 KB of RAM. The serial receive buffer is raised with a
  build flag so a block arrives whole before any of it is put on the bus.

The serial rate caps throughput at 100 KB/s, less the per-block
acknowledgements, so all 64 KB of VRAM takes one to two seconds. Bursts from the
Nano's buffer run faster than any 6502.

### Video capture

HDMI dongle → HDMI capture card → Mac. `vdpctl grab` runs `ffmpeg -f
avfoundation` and saves a PNG. The first run triggers macOS's camera permission
prompt for the calling app. `vdpctl compare-capture` downsamples a grab to
320 × 240, maps each pixel to the nearest palette entry, and reports the match
rate against a golden or snapshot, with a mismatch image.

Capture can never be exact — the dongle resamples the DAC's output on its way to
HDMI, and the card compresses — so it is **never the pass/fail oracle** (the
snapshot is). It proves what the snapshot cannot see: sync lock, DAC bit order,
line doubling, a picture that stays stable under load. It is used in Phases 9,
10, 13 and 14.

### Host tools (`tools/`)

Node ESM scripts with no build step, as in the emulator's `scripts/`.
`serialport` talks to the Nano and the debug link.

| Tool | Does |
|---|---|
| `vdpctl flash \| info \| stats \| snapshot \| vram \| inject \| reset \| reboot` | debug link |
| `vdpctl bus \| replay \| sweep \| irq-timing` | Nano harness |
| `vdpctl grab \| compare-capture` | capture |
| `vdpctl compare` | a snapshot against a golden; writes PNGs of both and of the difference |
| `fuzz.mjs` | `Video.ts` against the core |
| `replay.mjs` | `tests/oracle`'s traces into the core (or `Video.ts`), each checkpoint against its golden: the host executor's Node path |
| `sync-oracle.mjs` | refresh `tests/oracle/` from the emulator: refuses unless its HEAD is on `origin/main` with a clean tree, takes only the PICOVDP's fixtures, and records the emulator commit, its `git describe --tags` and SHA-256s in the manifest |

---

6. Phases
---------

Part A needs no PRO. Part B starts when it arrives. Part C is the machine itself.
Phases 1 and 2 are independent and may run in either order or together.

### Part A — Before the PRO

### Phase 0 — Repository and toolchain

*Nothing is built yet; everything after this builds on it.*

- `git init` on `main`. Add `README.md`, `.gitignore` and `THIRD_PARTY.md`.
  `LICENSE` is MIT, already in place. Files taken from pico9918 keep their MIT
  headers (Appendix A).
- **Toolchain.** Homebrew lists `gcc-arm-embedded` as installed, but its files
  are missing, so reinstall it (`brew reinstall --cask gcc-arm-embedded`, which
  asks for the owner's password). Add pico-sdk 2.1.1 as `external/pico-sdk`, with
  `lib/tinyusb` initialised. `picotool` 2.3.0, CMake, Ninja, `ffmpeg`, Node and
  PlatformIO are already present.
- `CMakePresets.json` with the five presets of section 3. The host presets build with
  `-Wall -Wextra -Werror`; `host-asan` adds AddressSanitizer and UBSan.
- `tools/package.json`; `bench/nano` PlatformIO skeleton.
- Pico 2 hello: a UF2 that logs over USB CDC.

**Done when:** `cmake --preset host && ctest --preset host` passes a placeholder
test; the `pico2` preset builds a UF2 that logs over USB; a second
`picotool load -x -f` reflashes the running Pico 2 with no button pressed.

### Phase 1 — Timing spike on the Pico 2 → **the budget, measured**

*SPEC §18 build-order step 2. The code is disposable; the numbers are not.*

- A `spike/` image, `copy_to_ram`, rendering on core 1. It runs at 302.4 MHz
  (PLL 1512 MHz ÷ 5, VREG 1.20 V) and 352 MHz (1056 MHz ÷ 3, 1.30 V), the presets
  pico9918 uses. Never at 252 MHz (§2).
- Synthetic worst-case line builds into a 320-byte buffer, then 640-pixel 12-bit
  expansion, timed with the DWT cycle counter. `time_us_32` resolves 1 µs against
  a ~26 µs build, too coarse. The scenes:
  - Full mode, two 4bpp layers with per-cell attributes and layer 1's transparency merge
  - 64-slot sprite evaluation
  - 32 × 16 × 16 sprites on the line, then magnified
  - detailed collision on and off
  - the same at 1, 2 and 8bpp
  - 4bpp with and without the 8 KB unpacking table
- Results to `docs/results/phase-01.md`, row for row against §18's table.

**Done when:** every row of §18's table has a measured figure at both clocks;
Still Open 2 (does the 4bpp table earn its 8 KB) has an answer; a clock preset is
chosen. **If the Full mode worst case leaves less than 25% of the line at
302.4 MHz**, Still Open 1's remedies are weighed, in order, before Phase 3 begins.

### Phase 2 — The oracle, exported → **traces replay without a CPU**

*Everything later is measured against what this phase produces. Nothing the
emulator does changes.*

In `6502-EMULATOR` (then on `v3-vdp`, since merged into `main`):

- An optional port observer on `Video`, called from `read` and `write`
  (`Video.read`/`Video.write` in `Video.ts`), plus a tick count since reset. It costs nothing while
  unset. `Machine.onRead`/`onWrite` are unsuitable: the debugger's
  `Session.syncBusTaps` reassigns them.
- `scripts/record-traces.mjs`: runs each fixture through `fixtures.js`'s
  `runFixture` with the observer attached and writes `<fixture>.vdpt.gz` to
  `docs/TRACE.md`'s format. Bus accesses happen on an instruction's first cycle,
  before that cycle's video tick. Timestamps record that. It runs the replay's
  analysis as it records and writes each checkpoint's class and settle point
  into the trace.
- `scripts/replay-trace.mjs`: a fresh `Video`, no CPU, ticked to each event. It
  asserts every read, writes each checkpoint's index frame, VRAM and JSON, and
  checks each checkpoint's class and settle point against the trace.
- `jest.picovdp.cjs` and `npm run test:picovdp`: `Video.test.ts`, unchanged, with
  `Video` mapped to the adapter named by `PICOVDP_ADDON`.
- Attaching the observer moves no golden; `npm test` stays green.

In this repo:

- `docs/TRACE.md`, format version 1.
- `tools/sync-oracle.mjs` and the first sync into `tests/oracle/`.
- `host/node`: the N-API adapter skeleton over an empty core.
- `tools/fuzz.mjs` skeleton.

**Done when:** replaying each fixture's trace reproduces all fifteen checkpoints
byte for byte — index frame, VRAM and JSON — with every recorded read matching;
every checkpoint has a class and a settle point; `npm run test:picovdp` enumerates all 342 tests
against the adapter (they fail — the core is empty).

### Phase 3 — Core: ports, registers, VRAM, palette

*SPEC §4, §5, §7, §11, §15. The bus side, and the render side's view of it.*

- **Ports (§4).** Two ports, each with pointer, direction latch, prefetch,
  flip-flop, payload latch and `STATSEL`. The prefetch rules follow §4's table
  exactly — a write loads the written byte into the prefetch. The pointer carries
  into the bank bits, and the stride is signed.
- **Registers (§5).** 7-bit decode, the `$02`–`$06` aliases, `VBANK` and `VINC`
  global, reserved registers stored with no effect.
- **VRAM (§7).** Bus and render copies, the journal and its overflow fallback,
  64 KB address wrap.
- **Palette (§11).** Window snooping, `PALBASE` reload, and the default palette
  transcribed from SPEC §11's table, not generated — rounding differs. Checked
  against `picovdp-default.pal`.
- **Reset (§15),** with power-on VRAM zeroed.
- Adapter surface for the above.
- C unit tests for what Jest cannot see: an operation between `vdp_line_start`
  and `vdp_build_line` does not reach that line.

**Done when:** the Jest blocks for direct VRAM access, the bus (§4, §5, VRAM,
`VBANK`/`VINC`) and the palette pass against the core; the fuzzer runs 10⁷
operations over ports, registers and VRAM with no divergence; `host-asan` is
clean.

### Phase 4 — Core: line timing, status registers, interrupts

*SPEC §3, §6, §14.*

- 262 screen lines and the latch (§3): the display line derived from the screen
  line at each line start, and moved 24 lines by a change of picture height.
  Vertical blank at the first line start of a frame past the picture's end, a
  frame beginning at screen line 0. Scanline compare. The once-a-frame guards for
  vblank, overflow and collision (§14).
- `STAT0`–`STAT15` (§6): sticky flags, the split acknowledgement, `/INT` =
  latches ∧ `IRQEN`, `MODE1` b5 ≡ `IRQEN` b0 (in the register file since Phase
  3), `STAT3` b0 from the line and b1 from the platform.
- Reset (§15): RST leaves the screen line and the frame's spent events alone.

**Done when:** the Jest block for timing, status and interrupts passes against
the core; the fuzzer, now with line starts and status reads, runs 10⁷ operations
with no divergence in reads or in `/INT` after any operation.

### Phase 5 — Core: the tile engine and the legacy submode → **`bios` goldens on the host**

*SPEC §8 at 1bpp, §9, §12 for layer 0.*

- Geometry table (resolved from the registers since Phase 3, for `textGrid`);
  legacy `M1`/`M2`/`M3` resolution and `L0ATTR × $40`; 1bpp with
  all four attribute sources; `COLOR`; transparency; backdrop and border per
  line; display off.
- Layer code written for both layers from the start — it is one engine (§8) —
  though only layer 0 is exercised here.

**Done when:** host replay of the `bios` trace reproduces `ok`, `screenful` and
`scroll` exactly; the Jest legacy block ("Video (TMS9918 VDP)") and the tile
engine's 1bpp, geometry and legacy-submode tests pass against the core, and so
does the palette block's "renders COLOR = $1F as black on white", which draws a
glyph and so moved here from Phase 3.

### Phase 6 — Core: sprites → **`wizardslab` goldens on the host**

*SPEC §9's sprite rules, §10, §12 against layer 0.*

- Evaluation: `SPRCOUNT`, the `$D0` terminator, `SPRLIMIT`, what counts toward
  the limit.
- `OVF` and `STAT7`; collision, and the detailed map behind `SPRCTRL` b3.
- Size, magnification, depths, flips, 9-bit X, negative Y.
- Priority among sprites, then the b6 level against the layers.
- The legacy semantics: Y + 1, `$E1`–`$FF`, the early clock, colour 0 invisible
  but colliding, no sprites in Text.

**Done when:** host replay of `wizardslab` reproduces all four checkpoints
exactly; the Jest sprites block passes against the core; the fuzzer's status
and tiles scopes, which hold sprites off until this phase (Phases 4 and 5), run
10⁷ operations with them on and no divergence in reads, `/INT` or frames.

### Phase 7 — Core: `VMODE`, bit depths, layer 1, scrolling → **the whole oracle on the host**

*SPEC §8 at 2, 4 and 8bpp, §9, §12, §13.*

- The attribute byte: flips, priority, pattern bit 8. Palette mapping and `LxPAL`.
  Layer 1. All seven priority levels. Per-pixel scrolling with 9-bit X.
- The 4bpp unpacking table, which Phase 1 found earns its place.
- `host/replay`: the pure-C replay CLI, under CTest. It compares index frames and
  VRAM exactly, plus registers and `STAT0` from the JSON. The full JSON, text grid
  included, is compared on the Node path.

**Done when:** all 342 Jest tests pass against the core, with any skips named and
justified; all fifteen checkpoints reproduce exactly through both `host/node` and
`host/replay`; the fuzzer, now building frames from random scenes, compares 10⁵
frames with no divergence.

### Phase 8 — Firmware on the Pico 2 → **goldens on silicon; the performance gate**

*Everything but the bus, on the same silicon as the PRO.*

- `firmware/`, `pico2` preset:
  - clocks at Phase 1's preset, 352 MHz
  - pico9918's VGA driver, VGA-only, on GPIO 0–13 (it runs without a monitor);
    line starts for all 262 lines
  - core 1's latch interrupt and renderer; core 0's sprite builds, with the
    column chosen per line (section 3)
  - late lines as SPEC §18 specifies them
  - explicit interrupt priorities
  - the start-up handshake fix
- The debug link, per `docs/DEBUGLINK.md` written here. Fault records, watchdog
  and safe mode. Statistics, including late lines: pico9918's VGA loop silently
  drops a stale line request; this one counts it.
- `vdpctl flash | info | stats | snapshot | vram | inject | reboot`.

**Done when:**
- all fifteen checkpoints reproduce exactly on the Pico 2 via `vdpctl inject`
  (index frame, VRAM, registers)
- the worst-case scenes of Phase 1, now drawn by the real core on both cores, run
  for ten minutes with zero late lines at `SPRLIMIT` 32, while `SNAPSHOT` streams
  continuously over USB; measured cycles are recorded against Phase 1 and §18
- a scene deliberately too heavy to build in time shows SPEC §18's late line — the
  previous line repeated — with its status unaffected, and the late lines are
  counted
- `vdpctl` triggers `FAULT`, reads the record from safe mode, and reflashes, with
  no hands on the board

### Part B — The PRO on the bench

### Phase 9 — Bench bring-up with the stock firmware → **wiring, harness and capture proven**

*Before any of this repo's firmware touches the PRO. If something fails here, it
is the bench.*

- **Owner:** fit the `MDE1` pin, mount the PRO, wire it per section 5 and
  `bench/hardware/picovdp-bench.kicad_sch`, connect the HDMI dongle and the
  capture card.
- Back up the PRO's stock flash first: `picotool save` in BOOTSEL, kept outside
  the repo.
- `bench/nano` primitives and scripts; `vdpctl bus`, `sweep`, `irq-timing`,
  `grab`, `compare-capture`; `docs/BENCH.md`.
- On the stock TMS9918 firmware, driven by the Nano:
  - text mode, with the 2 KB character set taken from `fonts/cp437-6x8.bin` (the
    same bytes as `BIOS.bin` 1.6 at `$B800`; BIOS 2.x has no font there)
  - write a line of text
  - read status
  - enable the vblank interrupt

**Done when:**
- the captured screen shows the text, matching a reference render within the
  capture tolerance
- 10⁶ random VRAM bytes are written and read back with no error at the
  `6502-1mhz` and `fastest` profiles (the stock firmware's 16 KB wrap applies)
- the `/INT` period is measured at 16.68 ms
- strobe-width and hold-time margins are recorded as the baseline for Phase 11
- the serial rate is confirmed through the Nano's USB bridge on macOS:
  1,000,000 baud, or the 500,000 fallback, recorded
- opening and closing the serial port 100 times, which resets the Nano each time,
  causes no stray access: VRAM and registers read back unchanged

### Phase 10 — The firmware on the PRO, no bus → **goldens by injection; the picture on the monitor**

*Phase 8 again, on the PRO, with its DAC and dongle attached.*

- `pro-debug` preset: `pico9918pro.h`, VGA output over the FFC to the HDMI
  dongle. The first flash needs BOOT; none after.

**Done when:**
- Phase 8's three criteria hold on the PRO
- each checkpoint's captured picture matches its golden within the capture
  tolerance — which proves the DAC's `0x0BGR` bit order on GPIO 2–13, line
  doubling and palette expansion
- a 256-entry palette test card is captured for the owner to judge the hue ramps
  (Still Open 3)

### Phase 11 — The bus → **four ports through real pins**

*SPEC §2, §4, §14's `/INT`, §15's RST. Section 3 sets out the PIO design's constraints.*

- `tmsWrite` unchanged; its handler decodes MODE1 and MODE to port and function.
- `tmsRead` rewritten for four staged bytes. The handler advances prefetches and
  acknowledges status; restaging happens on every status-affecting event.
- `/INT` on GPIO 22; RST on GPIO 23 performs §15.
- Bus FIFO overruns counted.
- `tests/bench/`: port conformance scripts drawn from the fuzz corpus, with
  expected reads from the reference replay, restricted to timing-independent
  operations.

**Done when:**
- 10⁷ random port operations over both ports — every command form, `VBANK` and
  `VINC` edge cases, and both read orders — run through the Nano with no read
  mismatch, at each of the three profiles
- back-to-back reads 4 µs apart return correct prefetch bytes
- strobe and hold margins are no worse than Phase 9's baseline
- a `/RESET` pulse yields §15's state, confirmed by `SNAPSHOT`, with `$0800`–`$0FFF`
  the built-in font (`VRAM`)
- no FIFO overruns

### Phase 12 — Traces through the bus → **every static golden via the Nano**

- `vdpctl replay`:
  - plays a trace's operations through the Nano, untimed
  - checks VRAM reads, and status reads that are timing-independent (`STAT4`–`STAT6`)
  - pauses at each checkpoint's settle point, takes the next complete frame with
    `SNAPSHOT`, then plays on to the checkpoint and reads VRAM and registers there

**Done when:** every static checkpoint reproduces exactly through the bus — index
frame from the settle point, VRAM and registers from the checkpoint. Phase 2
found all fifteen static. A checkpoint that becomes dynamic after a re-sync is
listed, with its Phase 10 injection result standing for it.

### Phase 13 — Raster timing, interrupts and load → **timing measured; SPEC.md updated**

*SPEC §3, §6, §14, §18, and Still Open 1 and 4.*

- **The latch.** A Nano script sets `IRQLINE` = N and, on `/INT`, writes `COLOR`
  and `L0SCRX`. The snapshot must show the change from line N + 2, on every one of
  10⁴ trials, with N swept through picture, border and blanking.
- **Interrupt timing.** vblank period and phase; scanline-compare offsets per
  geometry; with the optional frame-sync tap, both relative to the raster — including
  where the odd VGA line falls.
- **Status freshness.** `STAT2` read from a scanline handler over 10⁴ interrupts
  gives the distribution of lag. `OVF` and `COL` publication delay, likewise.
- **Load.** The worst-case scene, plus the Nano's fastest traffic on both ports,
  plus a scanline interrupt every eight lines, plus `FONT` written for both
  layers every frame (`vdpctl load --fonts`), plus USB snapshot streaming, for
  thirty minutes. `latch_isr_max` is recorded: on the Pico 2 the two loads take
  the latch interrupt from about 350 to about 3,200 cycles
  (docs/results/draft-0.5.md), and back-to-back accesses at `fastest` across
  vertical blank show whether a bus interrupt can wait that long.
- **Release parity.** `pro-release` repeats Phases 11 and 12 through the bus,
  using VRAM readback and capture in place of the snapshot.

**Done when:**
- the latch holds on all trials
- the load run shows zero late lines, zero FIFO overruns and a stable capture
- the release build matches
- the measurements are in `docs/results/phase-13.md` and in SPEC.md §3, §14, §18
  and Still Open — all three copies

### Part C — The machine

### Phase 14 — In the AC6502 → **the unmodified BIOS boots**

- The PRO, with its `MDE1` pin, fitted to the AC6502. `pro-release` with `STAT5`
  set to the release version.
- §16's detection probe returns carry set.

**Done when:**
- the unmodified BIOS 1.6 reaches `OK` in its legacy text mode; no golden holds
  this on the PICOVDP, so it is judged on the bench
- BIOS 2.0 reaches `OK`: it detects the card by `STAT4` `$AC` and `STAT6` b7, and
  the `screenful` and `scroll` inputs typed by hand show what the `bios` goldens
  show, the scroll done in hardware through `L0SCRY`
- `graphics-1.asm` draws what it drew
- `VdpModes.crt` and `VdpLayers.crt` run and match their goldens' pictures within
  the capture tolerance, at matching visual states (frame numbers will not line
  up, §18)
- the release is tagged, and `6502-EMULATOR`'s plan is told its merge condition
  is met

### After this plan

- **`6502-BIOS`:** hardware `VideoScroll`, port B in interrupt handlers, new entry
  points (SPEC §17).
- **`6502-EMULATOR`:** `v3-vdp` is merged and released as 3.0.0; the documentation
  rewrite in `6502-DOCS` follows.

---

7. Risk Register
----------------

1. **The line budget does not hold in Full mode.** Phase 1 measured it: on one
   core it did not, at either clock. Phase 8 measured the firmware itself, each
   row in two halves, at 352 MHz: with no bus every worst case fits for ten
   minutes, with 10% spare at `SPRLIMIT` 32 and 31–40% at 16. Under Phase 1's bus
   stand-in, a write every 2 µs on core 1, `SPRLIMIT` 16 fits with 16–24%, but at
   32 ten of the 36 worst cases make late lines. What is left is the real PIO
   handler (Phase 11) and the PRO's thermals at 1.30 V (Phases 10, 13; the Pico 2
   ran 21 minutes clean). If the margin erodes, a late line is specified; the
   renderer has evaluation, the split's choice and the judged layer's contest
   left to optimise (docs/results/phase-08.md, "For later phases").

2. **The §3 latch cannot be exact while the bus is served on the render core.**
   The dual VRAM and journal design exists for this. Phase 3 unit-tests it; Phase
   13 tests it on the pins. What remains is a journal overflow under traffic no
   6502 can produce, which falls back to page copies and is counted.

3. **The debug link disturbs the raster.** TinyUSB interrupts share core 0 with
   the VGA interrupts and core 0's half of each row. Since Phase 8 that half runs
   in an interrupt above USB and the link in thread mode, and ten minutes of
   continuous snapshots on the worst case cost no late line; late lines are
   counted; Phases 10 and 13 load the link while measuring; and a release build
   without USB must match. If it cannot be tamed, the link moves to snapshots taken
   only while the raster is idle — sacrificing the load test, not the oracle.

4. **An image dies before USB comes up.** Then the BOOT button is the only way
   back. The debug build brings USB up straight after the clocks, and the
   bring-up code before that stays as small as pico9918's.

5. **The read PIO cannot serve four ports in time.** Four staged bytes plus a
   branch on two pins is a few more PIO instructions at 3.3 ns each, against a
   6502's hundreds of nanoseconds. The real risk is restaging after a read before
   the next one, 2–4 µs later. Phase 11 measured both. The first program, a loop
   that shifted to the port's byte, was 20 ns too slow for the harness's floor;
   the one that shipped chooses its byte through a jump table during the
   debounce and drives the bus 43–48 ns after `/CSR` falls. Back-to-back data
   reads 4 µs apart are always right; 2 µs apart, about 1 in 170 is stale,
   because a read landing in core 1's latch interrupt (up to 1.6 µs) waits for
   it before its restage. The plan requires 4 µs; 2 µs is Phase 13's to weigh
   (docs/results/phase-11.md, "Back to back").

6. **Status is stale.** The status byte is staged before the read arrives (§6).
   Phase 13 measures by how much. If it is more than a line, §6 says so rather
   than the firmware pretending.

7. **Emulator timing is not the PRO's.** 60 Hz against 59.94, and no odd VGA
   line. Injection is addressed by frame and line count, and the bus replay is
   untimed, so neither depends on it. Timing is judged against SPEC.md, never
   against the emulator's clock.

8. **The oracle drifts.** The emulator keeps changing on `main`. The manifest
   pins its commit, `sync-oracle.mjs` refuses a dirty or wrong-branch tree, and
   re-syncs are commits of their own (rule 4).

9. **The Nano is not a 6502.** Its strobes and spacing come from profiles, not
   from a PHI2. Margin sweeps bracket the 6502's timing from both sides, and
   Phase 14 is the check that the real machine agrees.

10. **The data bus is wired bit-reversed.** CD0 is the MSB (section 5). Phase 9's
    readback test catches it before any firmware of ours is involved.

11. **SDK drift.** pico9918 documents failures under pico-sdk 2.2.0. The
    submodule is pinned at 2.1.1, and upgrading is a decision, not an accident.

12. **The Nano runs out of room.** 2 KB of RAM caps blocks and scripts at 256
    bytes. Its one input-capture pin serves `/INT`, leaving frame sync to a
    pin-change interrupt. If a script outgrows that, or frame-sync timing needs
    better than a few µs, the harness moves to the Mega 2560 on hand. It is the same
    16 MHz AVR with the same protocol and profiles; only the pin map, block size
    and capture timers change, and the host tools are untouched.

---

8. What This Plan Owes SPEC.md
------------------------------

### Still Open

| Item | Answered by |
|---|---|
| 1. Time Full mode first | Resolved in draft 0.4 by Phase 1. Phase 8 re-measured it with the firmware, recorded in §18; Phase 13 measures under bus load on the PRO |
| 2. Does the 4bpp table earn its 8 KB | Resolved in draft 0.4 by Phase 1: yes |
| 3. Are the hue ramps usable | **Resolved in Phase 10: yes.** The palette test card went on the PRO and was captured off its DAC (`docs/results/phase-10/shots/`) — all 256 entries reach the monitor and not one step of the sixteen families fails to brighten — and the owner judged the ramps good as they stand on 2026-09-23. SPEC.md's measurement item 3 is struck through with it |
| 4. How fresh the status byte can be | Phase 13 |

### Found while planning — settled in draft 0.3

Both were about the raster, which the emulator does not have, so they did not
come up when draft 0.2 was checked against it. SPEC.md, the emulator's
`docs/VDP-SPEC.md` and the published HTML were changed together (rule 2), and
`Video.ts` on `v3-vdp` implements the result, with six tests for it. No golden
moved.

1. **Where the odd VGA line is.** Draft 0.2 put it after display line 261, which
   in Text and Compact is a visible top-border row. It follows screen line 261 in
   every mode: display line 261 in Graphics and Full, 237 in Text and Compact.
2. **The line counter across a change between 192- and 240-line geometries.**
   Draft 0.2 said the counter was not re-based, which a fixed raster cannot do.
   Draft 0.3 counts **screen lines** from the top of the frame and derives the
   display line from them, so a change of picture height moves the display line
   by 24 at the next line start. A frame begins at screen line 0, and vertical
   blank fires exactly once in it however the height changes. Reset leaves the
   raster running.

### Measured in Phase 1 — settled in draft 0.4

SPEC.md, the emulator's `docs/VDP-SPEC.md` and the published HTML were changed
together (rule 2). `Video.ts` on `v3-vdp` implements the result: one reset value
and `STAT5`. The structural goldens moved by that register value, in a commit of
their own; no index or VRAM golden moved.

1. **The sprites move to core 0**, and §18's budget is measured, not estimated.
   The design is budgeted at 352 MHz.
2. **`SPRLIMIT` resets to 16**, not 32, which keeps 25% of the worst line spare.
3. **A late line is specified:** it shows the most recently completed line
   again, and its status, and everything else, are unaffected.

### The built-in font — settled in draft 0.5

Planned in `VDP-PLAN.md` (local) with the emulator, BIOS and include plans, not
found by a phase. SPEC.md, the emulator's `docs/VDP-SPEC.md` and the published
HTML were changed together (rule 2). The core, `Video.ts` and the firmware
implement it after the spec; no existing golden moves, so a `vdp-font` fixture
proves it.

1. **Reset loads font `$00`**, CP437 6 × 8, to `$0800`–`$0FFF` after the palette,
   at power-on and at every `RST`. The bytes are `fonts/cp437-6x8.bin`, pinned by
   SHA-256 (§7, §15).
2. **Register `$30` `FONT`** loads it into layer 0's or layer 1's pattern table,
   the base sampled at the write. The load completes at the line start where
   vertical blank fires, before F sets: a 2 KB copy does not fit an access, and
   there is no status register left to report it (§5, §7, §14).
3. **`STAT6` b7** advertises both, so `STAT6` reads `$BF`; the emulator's `STAT5`
   reads `$05` (§6, §16, §18).

### Decisions this firmware records, which SPEC.md may want to state

- Power-on VRAM is zeroed (§15 leaves it undefined; the emulator's cold start does
  the same). RST does not zero it.
- `STAT5` reads the firmware version; the first release's value is set in Phase 14.
- `OVF` and `COL` are published when core 1 finishes the row's build, not at its
  latch (Phase 8). Their lag is measured in Phase 13.
- A status read served a byte staged before a flag set acknowledges only what it
  showed: a `STAT0` read clears the flags in the byte it returned, with what
  details each and its `STAT1` latch, and a `STAT1` read the latches in its
  byte. A flag the CPU never saw stays set (Phase 11). §6 says a read clears the
  flags; it does not say which read, when the read and the card disagree.
- RST acts on its falling edge, taking any accesses already waiting first
  (Phase 11).

---

Appendix A — What Comes From pico9918
-------------------------------------

`/Users/acwright/Developer/C/pico9918`, MIT (firmware) — headers kept, and listed
in `THIRD_PARTY.md`.

| pico9918 | Here | Changes |
|---|---|---|
| `src/vga/` (`vga.c`, `vga.h`, `vga.pio`, `vga-modes.c`) | `firmware/vga/` | VGA 640 × 480 only (interlace, SCART and the other modes removed); a PIO interrupt at every screen line's start; the RGB buffer chosen per row at its line start, which is where late lines are shown; explicit priorities. `vga.pio` unchanged |
| `src/pio-utils/` | `firmware/pio-utils/` | none |
| `src/tms9918.pio` `tmsWrite` | `firmware/bus.pio` | none |
| `src/tms9918.pio` `tmsRead` | `firmware/bus.pio` | rewritten for four ports (section 3) |
| `src/main.c` bus handlers, `/INT`, RST, clock presets | `firmware/bus.c`, `firmware/main.c` | rewritten around the core; clock presets reused; RST no longer reads flash in an ISR |
| `src/boards/pico9918pro.h` (BSD-3-Clause, from the SDK) | `firmware/boards/` | none |
| `src/gpio.h` pin map | `firmware/pins.h` | MODE1 used |
| `vrEmuTms9918` | — | not used (SPEC §18) |
| `gpu/`, `config.c`, `flash.c`, `splash.c`, `diag.c`, `palconv.pio`, `configtool/`, SCART detection | — | not used |
| `test/host` — a Pico driving the TMS9918 bus by GPIO | — | not used, but the nearest precedent for `bench/nano` |
