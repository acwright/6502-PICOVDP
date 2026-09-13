Phase 3 — Core: ports, registers, VRAM, palette
===============================================

**Status:** done on 2026-09-13. Every check below passed, with one test of the
palette block moved to Phase 5 because it draws a text glyph. See
[Differences from the plan](#differences-from-the-plan), item 4.

**The headline.**

- **The bus side is in C.** Four ports, the 128-register file, 64 KB of VRAM
  and the palette are implemented, per §4, §5, §7, §11 and §15.
  `Video.test.ts`'s blocks for them pass against the core: the VDP bus 23 of
  23, direct VRAM access 2 of 2, the palette 14 of 15. 116 of the 342 tests
  pass in all, up from 4.
- **The latch works.** The core keeps a bus side and a render side. The
  render side catches up at `vdp_line_start`: it replays a journal of VRAM
  writes, or copies whole pages when a line's journal fills. A unit test
  checks that a write made between a line's latch and its build does not
  reach that line. Another sends 400,000 random operations with a check after
  each of 58,931 latches; 726 of those latches overflowed the journal.
- **The fuzzer finds no divergence.** Two seeds of 10⁷ operations each ran
  against `Video.ts`, with no divergence in reads, registers, port state,
  palette or VRAM. Each run made 2.9 million line starts. Planted bugs are
  caught and minimised to one to three operations.
- **`host-asan` is clean.**

Contents: [What was built](#what-was-built) · [The two sides](#the-two-sides) ·
[Video.test.ts against the core](#videotestts-against-the-core) ·
[Unit tests](#unit-tests) · [The fuzzer](#the-fuzzer) ·
[Snapshots](#snapshots) · [Done when](#done-when) ·
[Differences from the plan](#differences-from-the-plan) ·
[Reproducing](#reproducing)

---

What was built
--------------

| Part | What it is |
|---|---|
| `core/vdp.h` | the section 3 interface, plus `vdp_init`. `vdp_t` now holds both sides: 135,456 bytes on the host and on the M33 |
| `core/bus.c` | §4, §5, §7: ports, the command protocol, the register file, VRAM writes and the journal |
| `core/line.c` | §15 reset and §3's latch: `vdp_line_start` drains the journal, copies dirty pages, snapshots the registers and refreshes the palette cache |
| `core/palette.c` | §11: the default palette, transcribed, and the render side's cache of paired 12-bit `0x0BGR` pixels |
| `core/geometry.c` | §9: the geometry a register file selects, and the legacy mode behind it |
| `core/render.c` | the build functions, which draw the backdrop for now, and `vdp_expand_line` |
| `core/vdp_internal.h` | register numbers, bits and helpers shared inside `core/` |
| `core/vdp_debug.h`, `debug.c` | inspection and snapshots: registers, VRAM, ports, the palette, the mode, statistics, save and restore. It is a library of its own, `picovdp_core_debug`, so a release image need not link it |
| `host/node/addon.c` | the binding for all of the above |
| `host/node/Video.cjs` | the adapter gains `getRegister`/`setRegister`, `getVramByte`/`setVramByte`, `readVRAM`/`writeVRAM`, `paletteBase`, `paletteEntry`, `portState`, `getMode`, `isDisplayEnabled`, `textGrid` and `serialize`/`deserialize` |
| `host/node/cp437.cjs` | the CP437 table behind `textGrid`, generated from the emulator's `CP437.ts` and checked identical |
| `tests/unit/` | `test_bus`, `test_latch`, `test_palette`, `test_reset`, and `card.h`. They replace Phase 0's placeholder |
| `tools/fuzz.mjs` | `--scope bus`, debugger operations in the stream, and a composition line in the log |

Nothing changed in `6502-EMULATOR`.

---

The two sides
-------------

**The bus side** is what `vdp_read` and `vdp_write` touch, at once. It holds
the two port pairs, the register file, and VRAM. A VRAM write lands in VRAM
and is appended to the journal. This happens in `vdp_poke`, which both data
ports, reset and a debugger's pokes all go through.

**The render side** is what the build functions read: a copy of the
registers, a copy of VRAM, and the palette cache. `vdp_line_start` brings it
up to date, in this order:

1. **Replay the journal** into the render copy of VRAM. A write that falls in
   the palette window, where the render side last had `PALBASE`, updates that
   cache entry.
2. **Copy the dirty pages.** Writes after the journal's 1,024 entries only
   mark their 1 KB page, and the latch copies each marked page whole. The
   window lies inside one page, so copying that page reloads the palette. Each
   latch that copies pages is counted.
3. **Snapshot the registers.** If `PALBASE` has moved, the whole window is
   re-read from the up-to-date copy.

Applied in that order, the cache always equals the window it ends on,
decoded. That holds even when `PALBASE` moves away and back within a line,
with writes to both windows in between (`test_latch`).

**Reset.** A power-on loads both sides directly, since nothing is building. A
warm reset (RST) resets registers and ports on the bus side, and rewrites the
palette window through `vdp_poke`. The line being built keeps its card, and
the next latch brings the reset through (`test_reset`). A snapshot restore
loads both sides directly, as a power-on does.

**Hot functions** are marked `VDP_HOT`. On the RP2350 that places them in
`.time_critical.<name>` without including an SDK header. The `pico2` build
shows `vdp_read`, `vdp_write`, `vdp_poke`, `vdp_register_write`,
`vdp_line_start`, `vdp_palette_cache_entry`, `vdp_build_layers` and
`vdp_expand_line` there.

---

Video.test.ts against the core
------------------------------

```
PICOVDP_ADDON=…/6502-PICOVDP/host/node/Video.cjs npm run test:picovdp
Tests:       226 failed, 116 passed, 342 total
```

| Block | Passed | Tests | Phase that completes it |
|---|--:|--:|---|
| **direct VRAM access** | **2** | **2** | 3 |
| **the VDP bus** | **23** | **23** | 3 |
| **the palette (§11)** | **14** | **15** | 5: one test draws a glyph |
| textGrid | 7 | 7 | — (debugger view, over §9's geometry) |
| Video (TMS9918 VDP) | 41 | 48 | 4, 5 |
| debugger accessors | 2 | 4 | 4 |
| frameIndices | 4 | 5 | 5 |
| display timing, status and interrupts | 5 | 29 | 4 |
| the tile engine (§8) | 5 | 108 | 5, 7 |
| sprites (§10) | 8 | 65 | 6 |
| two layers, priority and scrolling (§12, §13) | 5 | 36 | 7 |

The one Phase 3 failure is **"the palette › the default palette › renders
COLOR = $1F as black on white"**. It draws a text-mode glyph and checks it is
black on a white border. The border is correct now. The glyph needs the 1bpp
tile engine, which is Phase 5.

Of the 226 failures:

- 39 call a Phase 4 accessor that still throws: `getStatus` 25,
  `getDisplayLine` 12, `peekStatus` 2.
- 187 are assertions on pictures, status or interrupts that the core does
  not produce yet.

---

Unit tests
----------

| Test | Checks | Covers |
|---|--:|---|
| `test_bus` | 55 | §4's prefetch table row by row; independent pairs; what resets the flip-flop; register writes leave pointers alone; `VBANK` sampling, the carry, wrap both ways, signed `VINC` (−1, −128, 0); the 7-bit decode, aliases, reserved registers, `MODE1` b5 ≡ `IRQEN` b0; `STAT4`–`STAT6` per port selector |
| `test_latch` | 30 | the operation between `vdp_line_start` and `vdp_build_line` that must not reach its line (backdrop register, palette entry, VRAM); journal overflow and page copies; `PALBASE` moving within a line; 400,000 random operations with the render side checked after every latch |
| `test_palette` | 2,335 | the core's table against **SPEC.md §11's printed table**, parsed from SPEC.md, and against **`picovdp-default.pal`**; the three half-way entries of row F; the table installed at `$FC00`; the window's edges and the ignored nibble |
| `test_reset` | 433 | power-on and RST: register reset values, ports, VRAM zeroed or kept, the palette window, RST reaching the render side only at the next latch |

`test_latch` passed on its first run, so it was mutated to show it can fail.
Each mutant changed `core/line.c` one way, and each was caught:

| Mutant | `test_latch` | `test_palette` |
|---|---|---|
| no palette update in the journal replay | 7 failed | 1 failed |
| dirty pages not copied | 7 failed | — |
| no reload when `PALBASE` moves | 6 failed | — |

---

The fuzzer
----------

`--scope bus` compares what Phase 3 owns:

- **every read of a data port**
- **every status read that selects `STAT4`–`STAT6`**, which are constants
- **every 5,000 operations:** all 128 registers, both port pairs, all 256
  palette entries and all 64 KB of VRAM

In this scope the stream also contains a debugger's register sets and VRAM
pokes. Status values, `/INT` and frames are not compared, because they belong
to Phases 4 and 5. Ticks stay in the stream, so line starts drive the latch
throughout.

| Run | Stream | Result |
|---|---|---|
| seed 1, 10⁷ | 8,103,496 writes, 1,445,124 reads, 44,505 register sets, 44,683 pokes, 5,170 resets; 183,006,917 ticks, 2,876,868 line starts | no divergence, 13.9 s |
| seed 2, 10⁷ | 8,101,311 writes, 1,445,763 reads, 44,573 register sets, 44,777 pokes, 5,280 resets; 183,602,491 ticks, 2,886,231 line starts | no divergence, 13.9 s |
| `--self`, seed 1, 10⁷ | as seed 1 | no divergence, 8.8 s |
| planted: `VINC` read unsigned | seed 1, 2 × 10⁵ | diverges at operation 2,255; minimised to 3, with port B's pointer named |
| planted: palette entry 255 misreported | seed 1, 2 × 10⁵ | caught at the first state check; minimised to 1 |
| `--scope all`, seed 7 | 10⁵ | diverges at operation 236 on a status read of `STAT2`, which is Phase 4's |

---

Snapshots
---------

Phase 2 left this open: carry snapshots in the core, or skip the three tests
that use them, by name. **They are carried.**

- `vdp_debug_save` and `vdp_debug_restore` move the registers, both ports,
  the screen line and VRAM.
- The adapter writes and reads `Video.ts`'s own `DeviceState` format.
- Nothing in `jest.picovdp.cjs` is skipped.

Where the three tests stand:

| Test | Result |
|---|---|
| the palette restore | passes |
| the raster restores ("restores the raster from a snapshot, and from one that predates it") | need `getDisplayLine`, which is Phase 4 |

A snapshot taken in `Video.ts` restores into the core exactly: registers,
ports, palette and all of VRAM. The reverse does not work yet. `Video.ts`
requires `stat0` and the other status fields, which the core's snapshot gains
in Phase 4.

---

Done when
---------

| Check | Result |
|---|---|
| the Jest blocks for direct VRAM access, the bus (§4, §5, VRAM, `VBANK`/`VINC`) and the palette pass against the core | ✅ 2/2 and 23/23; palette 14/15, the fifteenth moved to Phase 5 (item 4 below) |
| the fuzzer runs 10⁷ operations over ports, registers and VRAM with no divergence | ✅ seeds 1 and 2, scope `bus` |
| `host-asan` is clean | ✅ `cmake --workflow --preset host-asan`: all 6 tests pass with ASan and UBSan, with `-fno-sanitize-recover=all` |
| C unit tests for what Jest cannot see: an operation between `vdp_line_start` and `vdp_build_line` does not reach that line | ✅ `test_latch`, shown able to fail |
| the default palette is transcribed from §11's table and checked against `picovdp-default.pal` | ✅ `test_palette`, against both |
| the host suite passes | ✅ `cmake --workflow --preset host`: 7 of 7 |
| firmware presets still build | ✅ `pico2`, `core/` with no warnings |

---

Differences from the plan
-------------------------

1. **`vdp_init(v, version)` joins section 3's interface.** `STAT5` is the
   firmware's version on the PRO and `$04` in the adapter (section 4). A power-on
   reset must not clear it, so it is given when the card is made.
2. **Inspection lives in `core/vdp_debug.h`, as the library
   `picovdp_core_debug`.** The adapter and the unit tests link it, and so will
   the debug link. A release image does not need to.
3. **Snapshots are carried, not skipped.** See [Snapshots](#snapshots).
4. **One palette test moves to Phase 5.** "Renders COLOR = $1F as black on
   white" draws a text glyph through the tile engine. Phase 5's "Done when"
   now names it.
5. **Three small pieces came forward from later phases.** Each is needed by
   something Phase 3 checks:
   - **§9's geometry resolution** (Phase 5's geometry table). The bus block
     reads the name table through `textGrid`, which needs the grid. The table
     of geometries is here; the renderer that uses it is still Phase 5's.
   - **`MODE1` b5 ≡ `IRQEN` b0** (listed in Phase 4). It lives in the register
     write path, and the fuzzer compares every register.
   - **`STAT4`–`STAT6`** (Phase 4). They are constants; the other thirteen
     status registers read 0 until Phase 4.
6. **Every line builds as the backdrop** until Phase 5. That is what §3's
   display off draws, and what the palette tests' border check needs.
7. **RST goes through the bus side.** Its palette rewrite is journaled and
   reaches the render side at the next latch, so a line mid-build is
   undisturbed. Power-on and restore load both sides at once.
8. **`paletteEntry` and `vdp_debug_palette` read the bus side.** They report
   the entry as the next line built will draw it, which the tests require at
   once. The render cache matches that value at the next latch; `test_latch`
   checks it does.
9. **`fuzz.mjs`'s `--frames-every` is now `--check-every`, and `--scope`
   selects what is compared.** The bus scope adds debugger operations, which
   a version 1 trace cannot hold, so their minimised cases are written as
   JSON only.
10. **The adapter's reset fills the back buffers with the backdrop**, as
    `Video.ts` does. Phase 2's filled them with zero, which would have shown
    in the first frame after a cold start.

---

Reproducing
-----------

```sh
cmake --workflow --preset host        # 7 tests: unit, oracle_pinned, spike_reference, node_addon_loads
cmake --workflow --preset host-asan   # the same less the addon, under ASan and UBSan
cmake --preset pico2 && cmake --build --preset pico2

node tools/fuzz.mjs --scope bus --seed 1 --ops 10000000
node tools/fuzz.mjs --scope bus --seed 2 --ops 10000000
node tools/fuzz.mjs --self --scope bus --ops 10000000

# 6502-EMULATOR, on v3-vdp, after npm run build:cli
PICOVDP_ADDON=$PWD/../../C/6502-PICOVDP/host/node/Video.cjs npm run test:picovdp
```
