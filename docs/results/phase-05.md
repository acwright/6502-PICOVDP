Phase 5 — Core: the tile engine and the legacy submode
======================================================

**Status:** done on 2026-09-13. Every check below passed.

**The headline.**

- **The `bios` goldens reproduce on the host.** The core replays `bios`'s trace
  and gives `ok`, `screenful` and `scroll` exactly: the index frame, all 64 KB
  of VRAM and the structural JSON. It also matches every read, line start and
  `/INT` change in the trace, all 232,995 events. Deciding each checkpoint's
  class again gives `static` for all three. `vdp-modes/text` reproduces too.
- **The tile engine is in C**, per §8 at 1bpp, §9 and §11's backdrop, for both
  layers. The legacy block ("Video (TMS9918 VDP)") passes 48 of 48 against the
  core. So do the palette block's "renders COLOR = $1F as black on white" and
  every 1bpp, geometry and legacy-submode test in the tile engine's block. 205
  of the 342 tests pass in all, up from 151.
- **The fuzzer compares pictures now.** A new `tiles` scope compares every
  frame either card presents. Two seeds of 10⁷ operations ran with no
  divergence, comparing 10,653 and 10,709 frames.
- **The checks can fail.** Fifteen mutants of the new code were planted, and
  `test_tiles` caught all fifteen. The fuzzer caught thirteen: the two it
  missed make the build read the bus side, which no Node-side check can see.
- **`host-asan` is clean,** and the `pico2` build has no warnings.

Contents: [What was built](#what-was-built) · [How a row is built](#how-a-row-is-built) ·
[The oracle on the host](#the-oracle-on-the-host) ·
[Video.test.ts against the core](#videotestts-against-the-core) ·
[Unit tests](#unit-tests) · [The fuzzer](#the-fuzzer) · [Mutants](#mutants) ·
[Done when](#done-when) · [Differences from the plan](#differences-from-the-plan) ·
[For later phases](#for-later-phases) · [Reproducing](#reproducing)

---

What was built
--------------

| Part | What it is |
|---|---|
| `core/tiles.c` | §8: `vdp_draw_layer`, one layer's display line. One engine for both layers: the layer number only picks a register block. Handles 1bpp, all four attribute sources, `COLOR`, `LxPAL`, transparency and `LxCTRL` b5, and §9's pins on layer 0 in the legacy submode (1bpp; per pattern group, or none in Text; `L0ATTR` × `$40`; index 0 always transparent). The cell walk takes §13's scroll, nine-bit X included. At 2, 4 and 8bpp the layer draws nothing yet |
| `core/render.c` | `vdp_build_layers` builds the whole 320-index frame row the latch named. It fills the backdrop, returns there for blanking, border and display off, then draws layer 0 and layer 1 over it at the geometry's origin |
| `core/vdp.h` | the render side gains `render_screen_line`, the screen line the build makes. `vdp_line_start` sets it to the line after its own, power-on sets it, and a snapshot restore sets it |
| `core/vdp_internal.h` | `LxCTRL`'s bits, the depth and attribute-source codes, `vdp_draw_layer` |
| `host/node/Video.cjs` | `tickCount` and `observer`, as `Video.ts` has them, so a trace recorder can watch the core |
| `tools/replay.mjs` | the host executor on the Node path. It replays `tests/oracle`'s traces into the core, or into `Video.ts` with `--reference`, and compares each checkpoint with its golden. `--classes` decides the classes again |
| `tools/lib/trace.mjs` | `TraceRecorder.checkpoint` |
| `tools/fuzz.mjs` | `--scope tiles` |
| `tests/unit/test_tiles.c` | 10 tests, 1,098 checks |
| CTest `replay_bios` | `tools/replay.mjs bios --classes` against the addon just built. It needs no emulator |

Nothing changed in `6502-EMULATOR`.

On the RP2350, `vdp_draw_layer` is in `.time_critical`, 580 bytes, beside
`vdp_build_layers` and `vdp_expand_line`.

---

How a row is built
------------------

`vdp_line_start(v, s)` latches the render side (Phase 3). It also records that
the build will make screen line *s* + 1: row 0 as screen line 261 begins (§3).
`vdp_build_line` then builds that row from the render side alone:

1. **The backdrop** fills all 320 indices: `L0PAL` × 16 + `COLOR` b3:0, as
   latched (§11).
2. **Blanking.** Screen lines 240–261 have no row. They build as the backdrop,
   which nothing shows.
3. **The border.** The row's display line comes from the geometry the latch
   took, the same geometry that numbered the line start (§3). A display line
   at or past the picture's height is border, and stays backdrop.
4. **Display off** leaves the picture as backdrop too (§3).
5. **Layer 0, then layer 1,** each if its `LxCTRL` b4 is set, into the
   picture's `width` indices at `origin_x`. Only layer 0 is pinned by the
   legacy submode. A transparent pixel is one not written. At 1bpp neither
   layer has a priority bit, so this order is all of §12's levels 1 and 3.

The layer walks cells, not columns. The scroll decides the map column that
screen column 0 falls in and how far into that cell it is. The first cell's
pattern byte is shifted by that much, and each cell after it starts at bit 7.
Text's cells are six pixels wide, so bits 1:0 are never reached. Every table
address is `uint16_t`, so it wraps at 64 KB (§7).

A cell whose two nibbles are both opaque takes a loop with no branch in it.
That is the BIOS console, and Wizards Lab's board. How fast the build is on
the M33 is Phase 8's to measure.

---

The oracle on the host
----------------------

`tools/replay.mjs` follows the emulator's `scripts/replay-trace.mjs` step for
step. It feeds the trace's reset, writes, reads and checkpoints in at their
ticks. The card makes its own line starts, `/INT` changes and read values, and
they are compared line for line, so the first read that differs stops that
fixture. At each checkpoint the tool compares the index frame, VRAM and
structural JSON with `tests/oracle`, byte for byte, along with the frame,
settle point and window the trace records.

`--reference` runs the same replay into `Video.ts`. All fifteen checkpoints
come out exact, with the classes decided again as `static`. That is the
harness's own check.

| Fixture | Checkpoint | Core | Why not yet |
|---|---|---|---|
| `bios` | `ok`, `screenful`, `scroll` | **exact**, all three `static` | |
| `vdp-modes` | `text` | **exact** | |
| `vdp-modes` | `compact`, `graphics`, `full` | index frame differs | 2, 4 and 8bpp: Phase 7 |
| `vdp-layers` | all four | index frame differs | 4bpp layers: Phase 7 |
| `wizardslab` | all four | stops at event 7,272: `STAT0` reads `$80` where the trace has `$D0`, which is `F` with sprite 16 dropped | sprites: Phase 6 |

---

Video.test.ts against the core
------------------------------

```
PICOVDP_ADDON=…/6502-PICOVDP/host/node/Video.cjs npm run test:picovdp
Tests:       137 failed, 205 passed, 342 total
```

| Block | Phase 4 | Phase 5 | Tests | Phase that completes it |
|---|--:|--:|--:|---|
| **Video (TMS9918 VDP)** | 47 | **48** | **48** | 5 |
| **the palette (§11)** | 14 | **15** | **15** | 5 |
| **frameIndices** | 4 | **5** | **5** | 5 |
| display timing, status and interrupts | 29 | 29 | 29 | 4 |
| debugger accessors | 4 | 4 | 4 | 4 |
| direct VRAM access | 2 | 2 | 2 | 3 |
| the VDP bus | 23 | 23 | 23 | 3 |
| textGrid | 7 | 7 | 7 | — |
| the tile engine (§8) | 6 | **43** | 108 | 6, 7 |
| sprites (§10) | 10 | 10 | 65 | 6 |
| two layers, priority and scrolling (§12, §13) | 5 | **19** | 36 | 6, 7 |

In the tile engine's block, every test in "1bpp, where the color byte is a
pair of nibbles", "the geometries VMODE selects" and the 1bpp half of "index 0
and LxCTRL b5" passes. So does "makes L0PAL name the sixteen colors a 1bpp
cell's nibbles index". "The legacy submode" passes 12 of 13, and "LxCTRL b4
and the backdrop" 3 of 4. Of its 65 failures, 63 draw at 2, 4 or 8bpp or read
an attribute byte, which is Phase 7. The other two draw sprites, which is
Phase 6.

In the layers block, all of layer 1 at 1bpp passes, and 13 of the 14 scrolling
tests, which draw 1bpp rulers. Of its 17 failures, 16 draw 4bpp layers to
contest priority, or set layer 1's depth. The seventeenth, "leaves sprites in
screen space", needs sprites.

---

Unit tests
----------

`test_tiles`: 10 tests, 1,098 checks.

| Test | Covers |
|---|---|
| `the_bios_console` | legacy Text with `COLOR` = `$1F`: six pixels of a cell and not eight, cell 39 at x 274, the side border, pattern row 1, the top border (display line 261), the bottom border and blanking |
| `a_nibble_of_zero_is_transparent_unless_index_0_is_opaque` | legacy Graphics I ignores `L0CTRL` b5 as reset leaves it; `VMODE` Compact honours it; `L0PAL` names the group and the backdrop's row |
| `each_attribute_source_finds_its_byte` | per cell, per pattern group, per pattern row and none, each finding its own byte among the other three; the legacy group table at `L0ATTR` × `$40` |
| `every_table_wraps_at_64kb` | a name table at `$FC00` whose row 29 is at `$0088`; a pattern at `$FFFF`; a per-row attribute at `$03FF` |
| `the_row_built_is_the_one_after_the_latch` | every one of the 262 latches in Compact and in Graphics: the row after the latch, its display line, border and blanking |
| `a_write_during_the_build_waits_for_the_next_line` | patterns, names, `COLOR`, `L0SCRX`, `L0CTRL`, `VMODE` and `MODE1` written between the latch and the build: the row is unchanged, and the next latch takes all of them |
| `display_off_and_a_disabled_layer` | `MODE1` b6; `L0CTRL` b4; layer 1 still draws, in `L1PAL`'s group, with the backdrop still in `L0PAL`'s |
| `layer_1_draws_over_layer_0` | a transparent layer 1 pixel shows layer 0, and a transparent layer 0 pixel the backdrop; `L1CTRL` b5 occludes; the legacy submode leaves layer 1 alone |
| `a_scroll_splits_a_cell_partway_into_its_pattern` | `L0SCRX` = 11 starts partway into a pattern byte; the ninth bit reaches a different cell; `L0SCRY` moves the pattern row |
| `a_restored_card_builds_the_row_it_was_saved_at` | `vdp_debug_restore` rebuilds the same row |

`a_write_during_the_build_waits_for_the_next_line` is the test only this suite
can make. The adapter builds each row the moment its latch falls, so neither
Jest nor the fuzzer can put a write between a latch and its build.

---

The fuzzer
----------

`--scope tiles` builds on the status scope, with the same stream and the same
state check every 5,000 operations. It adds:

- **every presented frame.** After every tick, if either card has presented a
  frame, both must have, and all 76,800 indices must agree. That is about one
  frame per 940 operations.
- **layers held at 1bpp.** After any operation that leaves `Video.ts`'s
  `L0CTRL` or `L1CTRL` b1:0 non-zero, both cards have those bits cleared by
  the same debugger register write. 2, 4 and 8bpp are Phase 7's. The legacy
  submode ignores `L0CTRL`'s depth, so it runs unheld.
- **sprites held off,** as in the status scope, until Phase 6.

Nothing else is masked. Scroll, `COLOR`, `LxPAL`, `VMODE`, the legacy mode
bits, display off and both layers' enables and tables are all in the stream.

| Run | Stream | Result |
|---|---|---|
| tiles, seed 1, 10⁷ | 8,103,496 writes, 1,445,124 reads, 44,505 register sets, 44,683 pokes, 5,170 resets; 183,006,917 ticks, 2,876,868 line starts; sprites held off 13,393 times, depth 21,284 times | no divergence, 18.2 s; 10,653 frames compared, 7,778 of them more than one colour |
| tiles, seed 2, 10⁷ | 8,101,311 writes, 1,445,763 reads, 44,573 register sets, 44,777 pokes, 5,280 resets; 183,602,491 ticks, 2,886,231 line starts; sprites held off 13,305 times, depth 21,200 times | no divergence, 18.5 s; 10,709 frames, 7,760 more than one colour |
| tiles, `--self`, seed 1, 10⁷ | as seed 1 | no divergence, 12.5 s |
| status, seed 1, 10⁷ (Phase 4's check, again) | as seed 1 | no divergence, 14.6 s |
| bus, seed 1, 10⁷ (Phase 3's check, again) | as seed 1 | no divergence, 14.3 s |

---

Mutants
-------

Each mutant changed one line of `core/tiles.c`, `render.c` or `line.c`. Each
was run against `test_tiles`, the `bios` replay, `Video.test.ts` (137 failures
unmutated) and 10⁶ operations of `--scope tiles`, seed 1. `test_tiles` missed
three of them at first: the partial cell's shift, the ninth scroll bit and
layer 1 pinned by the legacy submode. It gained
`a_scroll_splits_a_cell_partway_into_its_pattern` and a legacy step in
`layer_1_draws_over_layer_0`, and the table shows the result after that.

| Mutant | `test_tiles` | `bios` replay | Jest failures | Fuzzer |
|---|---|---|--:|---|
| the partial cell's pattern not shifted | 1 failed | exact | 137 | op 3,028, minimised to 10 |
| legacy `L0ATTR` × `$400` | 2 failed | exact | 143 | op 18,902, minimised to 8 |
| legacy honours `L0CTRL` b5 | 1 failed | exact | 138 | op 17,194, minimised to 8 |
| per pattern group by pattern >> 4 | 1 failed | exact | 143 | op 18,902, minimised to 5 |
| per pattern row ignores the row | 2 failed | exact | 142 | op 67,810, minimised to 12 |
| `LxPAL` left out of the foreground | 2 failed | exact | 139 | op 3,028, minimised to 10 |
| the opaque loop tests b6 | 4 failed | **differs** | 165 | op 3,028, minimised to 6 |
| the ninth scroll bit ignored | 1 failed | exact | 138 | op 17,194, minimised to 10 |
| the layer reads the bus registers | 1 failed | exact | 137 | **no divergence** |
| the layer reads the bus VRAM | 1 failed | exact | 137 | **no divergence** |
| the row numbered as the screen line | 5 failed | **differs** | 157 | op 3,028, minimised to 6 |
| display off ignored | 1 failed | exact | 137 | op 261, minimised to 2 |
| layer 1 pinned by the legacy submode | 1 failed | exact | 138 | op 17,194, minimised to 7 |
| the latch builds its own line | 8 failed | **differs** | 189 | op 3,028, minimised to 6 |
| Text drawn eight pixels a cell | 1 failed | **differs** | 142 | op 3,028, minimised to 6 |

The two the fuzzer misses read the bus side instead of the render side. The
adapter builds a row the instant its latch falls, when the two sides are
equal, so nothing on the Node path can tell them apart. That is why
`a_write_during_the_build_waits_for_the_next_line` exists. The `bios` replay
catches only mutants that change Text at 1bpp, which is all `bios` draws.

The sources were restored afterwards, and every check above passed again on
them.

---

Done when
---------

| Check | Result |
|---|---|
| host replay of the `bios` trace reproduces `ok`, `screenful` and `scroll` exactly | ✅ `tools/replay.mjs bios --classes`: index frame, VRAM and JSON exact; every read, line start and `/INT` change matched; all three `static`. CTest `replay_bios` |
| the Jest legacy block ("Video (TMS9918 VDP)") passes against the core | ✅ 48/48 |
| the tile engine's 1bpp, geometry and legacy-submode tests pass against the core | ✅ 1bpp 4/4, geometries 6/6, legacy submode 12/13. The thirteenth, "draws them in VMODE's Text geometry", draws sprites (Phase 6) |
| the palette block's "renders COLOR = $1F as black on white" passes | ✅ the palette block 15/15 |
| the host suite passes | ✅ `cmake --workflow --preset host`: 10 of 10 |
| `host-asan` is clean | ✅ `cmake --workflow --preset host-asan`: 8 of 8 under ASan and UBSan |
| firmware presets still build | ✅ `pico2`, no warnings |

---

Differences from the plan
-------------------------

1. **Scrolling is in, at 1bpp.** The plan gives §13 to Phase 7. The cell walk
   is the same loop with or without a scroll, so it takes the scroll registers
   now. 13 of the 14 Jest scrolling tests pass. The fourteenth needs sprites.
   Phase 7 still owns scrolling at the other depths, which use the same walk.
2. **Layer 1 draws at 1bpp.** The plan says layer 1 would be "written but not
   exercised". It is exercised: Jest's layer 1 block passes at 1bpp, and so do
   `test_tiles` and the fuzzer. Painting layer 1 over layer 0 is all of §12
   that 1bpp needs. The seven-level compositor is still Phase 7's.
3. **The host replay runs on the Node path, as `tools/replay.mjs`.** The plan
   says "host replay" and gives `host/replay`, the pure-C CLI, to Phase 7. The
   Node tool is the executor Phase 7 names for comparing the full JSON, and it
   is what checked this phase. For it, the adapter gained `tickCount` and
   `observer`, both part of `Video.ts`'s public surface.
4. **The fuzzer gains a `tiles` scope** that compares frames, holding layers
   at 1bpp and sprites off. The plan does not ask for it until Phase 7.
5. **`vdp_t` gains `render_screen_line`.** The render side needs to know which
   row it builds, without reading the bus side's `screen_line`.
6. **No 6502-EMULATOR changes.**

---

For later phases
----------------

- **Phase 6.** `vdp_build_line` is still only `vdp_build_layers`. The sprites
  draw into the picture `vdp_build_layers` leaves, at the geometry's origin,
  and not in legacy Text (§9). `tools/replay.mjs wizardslab` is the done-when
  check. The `tiles` and `status` scopes should then run with sprites on, and
  the legacy-submode test that draws sprites should pass.
- **Phase 7.** `vdp_draw_layer` returns early at 2, 4 and 8bpp. The attribute
  byte, flips, priority and the 4bpp table go there, and the compositor goes in
  `vdp_build_layers`, which at present paints layer 1 over layer 0. Lift the
  `tiles` scope's depth hold. Add `vdp-modes` and `vdp-layers` to the
  `replay_*` CTests.
- **Phase 8.** The 1bpp loop is plain C, one byte of output per iteration.
  §18 describes a 64-byte nibble-mask table for 1bpp. Whether it is needed is
  a question for the cycle counter.

---

Reproducing
-----------

```sh
cmake --workflow --preset host        # 10 tests: unit (bus, latch, palette, reset, status, tiles), oracle_pinned, spike_reference, node_addon_loads, replay_bios
cmake --workflow --preset host-asan   # the same less the addon and the replay, under ASan and UBSan
cmake --preset pico2 && cmake --build --preset pico2

node tools/replay.mjs bios --classes          # the three bios goldens, exact
node tools/replay.mjs --reference --classes   # all fifteen into Video.ts: the harness's check
node tools/fuzz.mjs --scope tiles --seed 1 --ops 10000000
node tools/fuzz.mjs --scope tiles --seed 2 --ops 10000000
node tools/fuzz.mjs --self --scope tiles --ops 10000000

# 6502-EMULATOR, on v3-vdp, after npm run build:cli
PICOVDP_ADDON=$PWD/../../C/6502-PICOVDP/host/node/Video.cjs npm run test:picovdp
```
