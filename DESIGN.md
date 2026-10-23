6502-PICOVDP — Design
=====================

How the firmware for the video card in [SPEC.md](SPEC.md) is built and checked: the ground
rules, the architecture, the oracle it is held to, and the bench it was proven on.

These sections were sections 2–5, part of section 8, and Appendix A of the firmware plan,
`PLAN.md`. That plan was complete with `v1.0.0` and was removed after commit `4650b8d`. Source
comments cite these sections by the plan's numbers, so the numbers are kept, and there is no
section 1, 6 or 7 here. The plan's scope, its phases (1–14) and its risk register are in git
history: `git show 4650b8d:PLAN.md`. Each phase's results are in `docs/results/phase-NN.md`.
Where the text below says "this plan" or names a phase, it is speaking from that time.

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
  SPEC.md  DESIGN.md  README.md  LICENSE  THIRD_PARTY.md
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
void    vdp_latch_take(vdp_t *v, uint16_t screen_line, vdp_latch_take_t *t); // ... its one moment (Phase 13)
void    vdp_latch_record(vdp_t *v, const vdp_latch_take_t *t, uint32_t tag); // ... and its record, interruptible
bool    vdp_latch_copy(vdp_t *v);                           // a step of the FONT copies a latch began (Phase 13)
bool    vdp_catch_up(vdp_t *v);                             // its render side, when the renderer is ready
bool    vdp_set_hblank(vdp_t *v, bool hblank);              // STAT3 b1, from the platform, §6; true if a port's answer moved
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
  interrupt as the line begins: the line's events (§14), and a record of where
  the journals end. `vdp_catch_up` runs in the renderer when it is ready for the
  line, which after a late line is later (§18), and brings the render side to
  exactly that record. Records wait in a ring of 8; a latch that finds it full
  is merged into the newest, and the line it held is never built. Since Phase 13
  its bus side is itself in parts: `vdp_latch_take`, the one moment the bus
  must not interleave with — the events, and where the journals end — and
  `vdp_latch_record`, which builds the record from what was taken and which the
  bus may interrupt.
- **Registers are journaled** as VRAM is (Phase 13): each write leaves its byte
  in a ring of 256, and a latch records where the ring ends, so the render side
  replays the writes before it. After a reset, or a ring that filled, the latch
  copies the register file whole instead. The derived per-line state (geometry,
  bases, scroll, palette base) is recomputed from the render side's registers.
- **A `FONT` load** lands at the latch where vertical blank fires (§7). The
  render side takes it from the record, at its place among the writes; the bus
  copy's 2 KB are copied by the renderer in the vertical blank's lines, in steps
  of 64 bytes, the bus served between the steps, and an access that reaches an unfinished copy
  finishes it first, so no access after the latch sees the load unfinished
  (Phase 13).
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
| 1 | The bus: the PIO's interrupt for every access, and RST. Restaging happens here only; anything else that moves what a port reads asks for one | highest on core 1 (`$00`) |
| 1 | Horizontal blanking, from the VGA timing: `STAT3` b1 (§6), restaged only when a port selects `STAT3` (Phase 13) | `$20` |
| 1 | The latch, from core 0's doorbell: `vdp_latch_take` for every line begun with the bus held off, then its record, the bus served meanwhile (Phase 13) | `$40` |
| 1 | The renderer: a `FONT` load's copy, a chunk at a time (Phase 13); `vdp_catch_up`, `vdp_split_choose`, post to core 0, `vdp_build_half` and `vdp_expand_half` for the rest, wait for core 0, `vdp_publish` with interrupts held off — into a buffer no line start can be sending. A row not finished when its line starts is late (SPEC §18): the line sends the last finished row again, and the build finishes for its status | thread |

Phase 1 put the sprites in core 0's thread and the debug link in its lowest
interrupt. Phase 8 swapped them: a USB write blocks, and a sprite build that USB
could preempt is a late line waiting to happen; in an interrupt above USB it
cannot be.

Phase 11 put the bus beside the latch at one priority, so that neither could
split the other's writes to status and the journals. A read landing in a latch
then waited for all of it, and at 2 µs that was stale about once in 170. Phase
13 put the bus above everything on core 1 and made what holds it off short:
the latch's moment, a `FONT` copy step and a row's publication each mask
core 1's interrupts for under 200 cycles, and nothing else does for long. The
bus's code and the latch's moment run from core 1's own scratch bank.

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
on hand as the fallback, in case the Nano runs out of room (the plan's risk 12).

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

**Phase 13 did without the tap.** It was to be the VGA dongle's VSYNC pin, and
the HDMI dongle has none. Every interval the Nano measures is between `/INT`
edges, and where `/INT` falls against the raster the card measures itself, on a
timer both its cores read (docs/BENCH.md section 4).

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

8. Decisions This Firmware Records
----------------------------------

Behaviour SPEC.md does not state, or did not when the plan was written.

- Power-on VRAM is zeroed (§15 leaves it undefined; the emulator's cold start does
  the same). RST does not zero it.
- `STAT5` reads the firmware version: `$10`, 1.0, is the first release (Phase 14).
- `OVF` and `COL` are published when core 1 finishes the row's build, not at its
  latch (Phase 8): 41 µs after the latch's line start on average and 60 µs at
  most under the load run (Phase 13), so always before the row is shown.
  SPEC.md §14 now gives the figure.
- A status read served a byte staged before a flag set acknowledges only what it
  showed: a `STAT0` read clears the flags in the byte it returned, with what
  details each and its `STAT1` latch, and a `STAT1` read the latches in its
  byte. A flag the CPU never saw stays set (Phase 11). SPEC.md §6 states it
  since Phase 13.
- A `FONT` load belongs to the line start where vertical blank fires, and every
  access after that line start finds it whole; the copy into VRAM is done behind
  it in chunks, and an access reaching an unfinished chunk copies that chunk
  first (Phase 13). SPEC.md §4 and §18 say so.
- `STAT3` b1 is set from the start of each VGA line's front porch to the end of
  its back porch, 160 pixels, on every VGA line, the blanking lines' included
  (Phase 13).
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
