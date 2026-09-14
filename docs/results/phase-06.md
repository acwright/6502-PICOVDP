Phase 6 — Core: sprites
=======================

**Status:** done on 2026-09-14. Every check below passed.

**The headline.**

- **The `wizardslab` goldens reproduce on the host.** The core replays
  `wizardslab`'s trace and gives `frame-60`, `frame-180`, `frame-300` and
  `frame-600` exactly: the index frame, all 64 KB of VRAM and the structural
  JSON. It also matches every read, line start and `/INT` change in the trace,
  all 1,159,492 events. Deciding each checkpoint's class again gives `static`
  for all four. The `bios` goldens still reproduce.
- **Sprites are in C**, per §10, §9's legacy sprites and §12 against 1bpp
  layers. The latch evaluates them and publishes overflow. The build draws them
  divided between the two cores, as the firmware will, and publishes collision.
  The sprites block of `Video.test.ts` passes 65 of 65 against the core. 267 of
  the 342 tests pass in all, up from 205.
- **Every division of a line builds the same line.** A unit test builds 780
  lines of random scenes at 16 split columns each and gets the same row and
  status every time. The adapter can also build every row twice, at the split
  the core picks and at another, and throw on any difference. Jest, both
  replays and a 10⁷-operation fuzz run all passed that way.
- **The fuzzer runs with sprites on.** The status and tiles scopes no longer
  hold sprites off, and the stream now writes sprite scenes. Four runs of 10⁷
  operations found no divergence in reads, `/INT` or frames. Each run saw
  Video.ts's `OVF` set about 1,400 times, `COL` about 1,300 times and the
  collision map grow about 750 times.
- **The checks can fail.** Twenty-one mutants were planted. One is equivalent.
  Of the other twenty, `test_sprites` catches all twenty and the fuzzer
  nineteen.
- **`host-asan` is clean,** and the `pico2` build has no warnings.

One thing to know about the oracle: **no `wizardslab` golden holds a sprite
pixel.** A core whose sprites draw nothing still reproduces all four exactly.
What the fixture checks is evaluation: the game polls `STAT0`, and the overflow
it reads has to match. The drawing is checked by Jest, the fuzzer and
`test_sprites` instead. `vdp-layers` draws sprites, and it is Phase 7's to
reproduce.

Contents: [What was built](#what-was-built) · [How a line's sprites are built](#how-a-lines-sprites-are-built) ·
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
| `core/sprites.c` | §10. `vdp_sprites_evaluate` runs at the latch. `vdp_build_sprites`, `vdp_draw_sprites` and `vdp_merge_sprites` draw, over any range of picture columns, with one worker. `vdp_split_choose` picks the column |
| `core/vdp.h` | `vdp_sprite_t` holds a sprite the latch found. `vdp_sprline_t` is now a real sprite line: level, index and owner per picture column, plus the collisions found. `vdp_t` gains the render side's sprite list and core 1's scratch: a level per column and a sprite line of its own |
| `core/render.c` | `vdp_build_layers` records each written pixel's §12 level on lines that have sprites. `vdp_build_line` divides the line the way the cores do |
| `core/tiles.c` | `vdp_draw_layer` takes a level buffer and a level |
| `core/line.c` | `vdp_line_start` evaluates the sprites last, from the render side it has just updated. Power-on evaluates them too |
| `core/debug.c` | `vdp_debug_restore` evaluates the restored line's sprites without reporting an overflow |
| `host/node/addon.c` | `buildLineAt(card, indices, split)` and `splitChoose(card)` |
| `host/node/Video.cjs` | `PICOVDP_SPLIT_CHECK`: every row built at two splits and compared |
| `tools/fuzz.mjs` | sprites on in the status and tiles scopes; sprite scenes in their stream; sprite event counts in the log |
| `tests/unit/test_sprites.c` | 14 tests, 25,952 checks |
| CTest `replay_wizardslab` | `tools/replay.mjs wizardslab --classes` against the addon just built |

The status and tiles unit tests gained `SPRCTRL` = `$26` where they turn the
display on. Power-on VRAM is zeroed, so all 32 slots sit at Y 0, overflow the
top lines and draw over the patterns those tests check. Sprites are now drawn,
so those tests have to turn them off.

On the RP2350 all of it is in `.time_critical`. The worker `draw_columns` is
888 bytes and `vdp_sprites_evaluate` 620.

Nothing changed in `6502-EMULATOR`.

---

How a line's sprites are built
------------------------------

**At the latch.** `vdp_line_start` drains the journal and copies the registers
(Phase 3). It then evaluates the sprites for the row it has just latched, from
the render side, in table order:

1. Nothing unless the row is on the picture, the display is on, the mode is
   not legacy Text and `SPRCTRL` b0 is set (§3, §9, §10).
2. For each of `SPRCOUNT` slots, at most 64: a Y of `$D0` ends the list while
   `SPRCTRL` b2 is set, and always in the legacy submode. Y is read as §10
   says, or as the TMS9918 did in the legacy submode (§9). Magnification
   doubles the lines a sprite covers.
3. A covering sprite past `SPRLIMIT`, at most 32, is an overflow. `STAT0`'s
   index field takes it if `OVF` was clear, `STAT7` takes it anyway, the
   frame's overflow interrupt fires once, and evaluation stops (§6, §14).
4. A covering sprite counts toward the limit wherever its X is. Only one that
   reaches into the picture is listed, with its left edge, the pattern row
   the line draws (vertical flip applied), its pattern and its attribute byte.

**In the build.** The layers come first. On a line with sprites they leave a
§12 level for each picture column: 0 for the backdrop, 1 for layer 0, 3 for
layer 1. The sprites are then drawn over a range of picture columns by one
worker. For each sprite on the list, it decodes the sprite's row into up to 16
pattern values, then walks the columns the sprite covers in the range:

- **Coverage and collision.** A non-zero value covers the pixel. The first
  slot to cover a pixel is its owner. A later one collides if `SPRCTRL` b1 is
  set, and with b3 both it and the owner go into the map. The lowest slot on a
  pixel is paired with each slot after it, so every sprite on a shared pixel is
  named, as `Video.ts` names them.
- **Priority among sprites.** The first slot to paint a pixel claims it at its
  level, 2, or 5 with b6. A legacy sprite of colour 0 covers without painting.
- **The pixel.** On core 1 it is written into the line if its level beats the
  layer's. On core 0 it goes into the sprite line with its level, and core 1's
  merge writes it on the same test.

Both halves decide everything pixel by pixel, from the render side and the
list, so no split can change the answer. Collision's only effects are to set
bits and spend a once-a-frame event, so publishing the halves in either order,
or one of them twice, gives the same status.

`vdp_split_choose` returns 0 when a line has no sprites. Otherwise it follows
Phase 1's spike: the sprites are spread evenly across the picture and the
split balances core 0's share against core 1's layers and the rest. The
result is rounded to a 32-pixel boundary and kept between half the picture
and all of it. The costs are the spike's, not measured on this renderer.

---

The oracle on the host
----------------------

| Fixture | Checkpoint | Core | Why not yet |
|---|---|---|---|
| `bios` | `ok`, `screenful`, `scroll` | **exact**, all three `static` | |
| `wizardslab` | `frame-60`, `frame-180`, `frame-300`, `frame-600` | **exact**, all four `static` | |
| `vdp-modes` | `text` | **exact** | |
| `vdp-modes` | `compact`, `graphics`, `full` | index frame differs | 2, 4 and 8bpp layers: Phase 7 |
| `vdp-layers` | all four | index frame differs | 4bpp layers and §12's layer levels: Phase 7 |

`PICOVDP_SPLIT_CHECK=1 node tools/replay.mjs --classes` gives the same table,
with every row built at two splits.

---

Video.test.ts against the core
------------------------------

```
PICOVDP_ADDON=…/6502-PICOVDP/host/node/Video.cjs npm run test:picovdp
Tests:       75 failed, 267 passed, 342 total
```

It gives the same result with `PICOVDP_SPLIT_CHECK=1`.

| Block | Phase 5 | Phase 6 | Tests | Phase that completes it |
|---|--:|--:|--:|---|
| Video (TMS9918 VDP) | 48 | 48 | 48 | 5 |
| the palette (§11) | 15 | 15 | 15 | 5 |
| frameIndices | 5 | 5 | 5 | 5 |
| display timing, status and interrupts | 29 | 29 | 29 | 4 |
| debugger accessors | 4 | 4 | 4 | 4 |
| direct VRAM access | 2 | 2 | 2 | 3 |
| the VDP bus | 23 | 23 | 23 | 3 |
| textGrid | 7 | 7 | 7 | — |
| **sprites (§10)** | 10 | **65** | **65** | **6** |
| the tile engine (§8) | 43 | **45** | 108 | 7 |
| two layers, priority and scrolling (§12, §13) | 19 | **24** | 36 | 7 |

The tile engine block gained the two tests that draw sprites: "draws them in
VMODE's Text geometry" in the legacy submode's tests, and "still draws sprites
over it". Each of its 63 remaining failures draws at 2, 4 or 8bpp or reads an
attribute byte.

The layers block gained "leaves sprites in screen space" and four priority
tests. Its twelve failures are one test of layer 1's depth and eleven of §12's
priority tests. Every one of those builds its layers at 4bpp (`contested()`
sets `BPP4` on both), which draw nothing until Phase 7. Of the four priority
tests that now pass, only "gives a 1bpp cell no priority bit" contests a layer
that draws. "Level 2", "level 5" and "draws nothing from a disabled layer" pass
because the 4bpp layer they contest draws nothing; the "sprite b6 ignored"
mutant below shows it. The same rules at 1bpp are in `test_sprites`.

---

Unit tests
----------

`test_sprites`: 14 tests, 25,952 checks.

| Test | Covers |
|---|---|
| `every_split_builds_the_same_line` | 60 random scenes. Each has a random mode, sprite size, depth, `SPRCTRL`, `SPRCOUNT`, `SPRLIMIT`, `SPRPAL`, 1bpp layers and VRAM, with 64 slots placed around a line. For 780 lines, the row and status built at 15 split columns (the 32-pixel ones and 1, 7, 100, 255, 319), and by `vdp_build_line`, equal core 1 building the whole line. Sprites were on 401 of the lines, collision on 271, overflow on 115 and a detailed map on 148, and the test checks that the scenes keep reaching them |
| `core_0s_collisions_wait_for_the_merge` | core 0's sprite line holds its collision and map. `STAT0`, the map and `/INT` stay clear through core 1's half and are set by the merge |
| `overflow_at_the_latch_collision_in_the_build` | `OVF`, its index, `STAT7` and its `STAT1` latch are set by `vdp_line_start` alone. `COL` waits for the build. A sprite's first row is the row after the latch before it |
| `no_sprites_outside_the_picture` | sprites wholly off the left still count toward `SPRLIMIT` on the picture's lines, and on no other. Sprites clip at the picture's right edge, not in the border. Nothing is evaluated with the display off or in legacy Text, but it is in `VMODE`'s Text |
| `sprites_stand_between_the_layers` | at 1bpp: over layer 0, under layer 1, over it with b6. Through a transparent layer 1 pixel; hidden by an opaque layer 1. The lowest slot keeps a pixel it loses to layer 1. The legacy submode ignores b6 |
| `legacy_and_vmode_read_y_differently` | legacy Y + 1 and `$F9` as −7, and `$D0` ending the list with `SPRCTRL` b2 clear. In `VMODE` modes: Y as the top edge, 249 as −7, and `$D0` as row 208 until b2 is set |
| `quadrants_flips_and_magnification` | 16 × 16 quadrants in TMS9918 order, rows 0, 7 and 8; horizontal and vertical flips of the whole sprite; magnification of pixels and rows |
| `stat7_follows_the_last_overflowing_line` | `STAT0`'s index field keeps the first sprite dropped, and `STAT7` names slot 36 from a later line |
| `the_early_clock_and_the_palette_mapping` | the legacy early clock and palette row 0, whatever `SPRPAL` holds. Outside it, X bit 8 and §10's mapping at 1, 2 and 4bpp |
| `a_legacy_colour_0_sprite_collides_unseen` | colour 0 draws nothing, shows the sprite behind it and collides, across the split column |
| `a_write_during_the_build_waits_for_the_next_line` | a slot, a pattern, `SPRCTRL` and `MODE1` written between the latch and the build don't reach the row, and do reach the next one |
| `the_sprite_tables_wrap_at_64kb` | `SPRPAT` `$FF` at 8bpp: pattern `$20` starts at `$0000` and `$1F` at `$FFC0`. Slot 63 of a table at `$7F80` |
| `a_restored_card_evaluates_without_reporting` | a restored card draws the saved line's sprites and does not raise the overflow it was saved with again |
| `the_split_is_a_word_boundary_in_the_picture` | in all four geometries, every size and count: 0 with no sprites, otherwise a multiple of 32 (or the picture's width) from half the picture to all of it |

Only this suite can check the split, the order in which status is published,
and a write between the latch and the build. The adapter builds each row the
moment its latch falls, on one thread.

---

The fuzzer
----------

The status and tiles scopes now run with sprites on, at every depth, size and
magnification. The layers are still held at 1bpp in the tiles scope.

**Sprite scenes.** Random bytes seldom make a sprite, so these two scopes add a
sprite scene to the stream at 2% of its picks. A scene is ordinary port
operations: sometimes `MODE1` with the display on, `IE`, `M1`, size and
magnification; sometimes a new `SPRATTR` or `SPRPAT`; then either up to 40
slots or up to 256 pattern bytes, mostly `$FF`. Slot Y is biased onto the
picture and its edges. The bus and all scopes draw no extra random numbers, so
their streams are unchanged from Phase 5.

**Showing sprites are reached.** After every operation, the log counts each
time Video.ts's `OVF` or `COL` goes from clear to set, and each time its
collision map gains a bit.

| Run | Stream | Result |
|---|---|---|
| tiles, seed 1, 10⁷ | 8,780,943 writes, 928,602 reads, 28,614 register sets, 28,838 pokes, 3,464 resets; 117,677,748 ticks, 1,849,894 line starts; `OVF` set 1,369 times, `COL` 1,329, the map grew 782 times; depth held 13,545 times | no divergence, 12.5 s; 6,855 frames compared, 5,658 of them more than one colour |
| tiles, seed 2, 10⁷ | 8,772,943 writes, 935,261 reads, 28,823 register sets, 28,983 pokes, 3,422 resets; 118,992,089 ticks, 1,870,555 line starts; `OVF` 1,402, `COL` 1,258, map 717; depth held 13,628 times | no divergence, 12.7 s; 6,928 frames, 5,653 more than one colour |
| status, seed 1, 10⁷ | as tiles seed 1 | no divergence, 11.5 s |
| status, seed 2, 10⁷ | as tiles seed 2 | no divergence, 11.6 s |
| tiles, seed 3, 10⁷, `PICOVDP_SPLIT_CHECK=1` | 8,774,068 writes, 933,933 reads; `OVF` 1,393, `COL` 1,298, map 702 | no divergence, 20.5 s; 6,920 frames |
| tiles, `--self`, seed 1, 10⁷ | as tiles seed 1 | no divergence, 9.4 s |
| bus, seed 1, 10⁷ (Phase 3's check, again) | as Phase 5 | no divergence, 16.1 s |

The status and tiles runs above were made on the final sources. The self,
bus and split-check runs were made before two changes that don't alter
behaviour: the worker's rename and `vdp_split_choose`'s `VDP_HOT`.

The `all` scope still diverges, as in Phase 5, because it doesn't hold layer
depth. Seed 1 diverges at operation 39,999 and minimises to 21 operations that
set `L1CTRL` to `$FF`, an 8bpp layer 1.

---

Mutants
-------

Each mutant changed one line of `core/sprites.c`, `render.c` or `tiles.c`. Each
was run against `test_sprites`, `test_tiles`, the `wizardslab` and `bios`
replays, `Video.test.ts` (75 failures unmutated) and 10⁶ operations of
`--scope tiles`, seed 1.

The first pass had nineteen mutants. `test_sprites` missed ten of them: the
legacy Y offset, the legacy terminator, `STAT7`, both flips, quadrant order,
the opaque loop's level, legacy `SPRPAL`, the early clock, and the bus-VRAM
mutant. It gained `legacy_and_vmode_read_y_differently`,
`quadrants_flips_and_magnification`, `stat7_follows_the_last_overflowing_line`,
`the_early_clock_and_the_palette_mapping`, and an opaque layer 1 step in
`sprites_stand_between_the_layers`. The misses were run again against it. Two
more mutants were added then: "a sprite off the picture not counted", whose
first version didn't compile, and "sprites draw nothing", to learn what
`wizardslab` checks. The `test_sprites` column is the final suite. The other
columns are from each mutant's full run.

| Mutant | `test_sprites` | `wizardslab` | Jest failures | Fuzzer |
|---|---|---|--:|---|
| legacy Y read without the + 1 | 2 failed | exact | 83 | op 7,647, minimised to 8 |
| `SPRLIMIT` + 1 drawn | 3 failed | **differs** | 84 | op 14,920, minimised to 3 |
| legacy ignores the `$D0` terminator | 1 failed | exact | 76 | op 9,701, minimised to 14 |
| a sprite off the picture not counted | 1 failed | exact | 75 | op 20,358, minimised to 10 |
| sprites draw nothing | 12 failed | exact | 132 | op 9,701, minimised to 4 |
| `STAT7` keeps the first overflow | 1 failed | exact | 76 | op 64,999, minimised to 12 |
| vertical flip ignored | 1 failed | exact | 76 | op 53,619, minimised to 212 |
| quadrants in reading order | 1 failed | exact | 76 | op 9,701, minimised to 22 |
| horizontal flip ignored | 1 failed | exact | 77 | op 53,619, minimised to 143 |
| collision on what was painted, not covered | 1 failed | exact | 76 | op 9,999, minimised to 4 |
| detailed map omits the lower sprite | 2 failed | exact | 81 | op 124,648, minimised to 299 |
| sprite b6 ignored | 1 failed | exact | 75 | op 53,619, minimised to 41 |
| a pixel lost to layer 1 is not claimed | 1 failed | exact | 75 | **no divergence** |
| the merge publishes nothing | 4 failed | exact | 87 | op 9,999, minimised to 4 |
| claims not cleared between lines | 3 failed | exact | 77 | op 16,865, minimised to 26 |
| layer 1 recorded at layer 0's level | 1 failed | exact | 75 | op 36,724, minimised to 41 |
| the opaque layer loop records no level | 1 failed | exact | 75 | op 36,724, minimised to 41 |
| legacy sprites take `SPRPAL` | 1 failed | exact | 76 | op 22,002, minimised to 60 |
| the early clock read as X bit 8 | 1 failed | exact | 76 | op 9,701, minimised to 13 |
| the group shifted by the depth, not the bits | 5 failed | exact | 113 | op 53,619, minimised to 97 |
| evaluation reads the bus VRAM | passed | exact | 75 | no divergence — **equivalent** |

- **Evaluation reading the bus VRAM is an equivalent mutant.** Evaluation runs
  inside `vdp_line_start`, just after the journal has brought the render copy
  level with the bus copy. On the RP2350 the latch and bus writes are core 1
  interrupts of one priority, so no write can come in between.
- **The fuzzer misses one mutant: a pixel a sprite loses to layer 1 not
  claimed.** It takes a lower slot under an opaque layer 1 pixel, with a
  higher slot at b6 on the same pixel. `test_sprites` has that case. Jest's
  version of it is a 4bpp test and fails for now anyway.
- **Jest misses what its 4bpp priority tests can't yet reach:** b6 and the
  layer levels. The fuzzer and `test_sprites` catch them.
- **`test_tiles` failed for the two layer-level mutants.** Its scenes turn the
  display on over zeroed VRAM, so sprites at Y 0 happen to sit under its
  layer 1.

The sources were restored after each mutant, and every check above passed again
on them.

---

Done when
---------

| Check | Result |
|---|---|
| host replay of `wizardslab` reproduces all four checkpoints exactly | ✅ `tools/replay.mjs wizardslab --classes`: index frame, VRAM and JSON exact; every read, line start and `/INT` change matched; all four `static`. CTest `replay_wizardslab`. Also exact with `PICOVDP_SPLIT_CHECK=1` |
| the Jest sprites block passes against the core | ✅ 65/65 |
| the fuzzer's status and tiles scopes run 10⁷ operations with sprites on and no divergence in reads, `/INT` or frames | ✅ seeds 1 and 2 in each scope, and seed 3 of tiles with the split check |
| the host suite passes | ✅ `cmake --workflow --preset host`: 12 of 12 |
| `host-asan` is clean | ✅ `cmake --workflow --preset host-asan`: 9 of 9 under ASan and UBSan |
| firmware presets still build | ✅ `pico2`, no warnings |

---

Differences from the plan
-------------------------

1. **The line's split is checked on the host now, not in Phase 8.** Section 3
   says "the reference check builds each line both ways". Here that is
   `every_split_builds_the_same_line`, the adapter's `PICOVDP_SPLIT_CHECK`, and
   `vdp_build_line` itself, which always builds divided.
2. **`vdp_split_choose` uses Phase 1's costs.** Section 3 has it correct itself
   from the last line's measured costs. There is nothing to measure on the
   host. Phase 8 adds the correction.
3. **The draw is per pixel, not in 32-bit words.** Phase 1's spike drew a word
   at a time with claim bitmaps, and §18's budget was measured on that. This
   worker is written to be checked, not timed. Whether it fits the line is
   Phase 8's question, and the spike's word version is the answer if it does
   not.
4. **§12's levels are in, at 1bpp.** A layer pixel records its level, and a
   sprite pixel is written only where its level beats it. The plan gives the
   compositor to Phase 7. That phase still owns layer 0's level 4 and layer
   1's level 6, and the layers' own contest with each other.
5. **The fuzz stream gains sprite scenes** in the status and tiles scopes, and
   the log counts sprite events. The bus and all streams are unchanged.
6. **`vdp_sprline_t` and `vdp_t` grow.** The sprite line is 976 bytes, and `vdp_t` 136,968; it holds
   a sprite list, a level line and core 1's own sprite line.
7. **No 6502-EMULATOR changes.**

---

For later phases
----------------

- **Phase 7.** `vdp_draw_layer` records one level for a whole layer. The
  attribute byte's b6 has to record 4 on layer 0 and 6 on layer 1, and layer 1
  must then yield to level-4 pixels of layer 0. Sprites need no change: they
  already compare against whatever level is recorded. Once layers draw at 4bpp,
  eleven Jest priority tests become live. Add `vdp-layers` to the `replay_*`
  CTests: it is the fixture that draws sprites into a golden.
- **Phase 8.**
  - `vdp_merge_sprites` and `vdp_draw_sprites` publish collisions from core 1's
    thread mode, while status reads run in core 1's bus interrupt. Their
    `stat0`, `frame_events`, `irq_latch` and map updates need the bus
    interrupt masked around them.
  - Core 0 posts nothing when `vdp_split_choose` returns 0.
  - Time `draw_columns` against Phase 1's word-at-a-time sprites. If it doesn't
    fit, port the spike's claim bitmaps; `every_split_builds_the_same_line`
    and the fuzzer are the checks for it.
  - Feed the split its measured correction.

---

Reproducing
-----------

```sh
cmake --workflow --preset host        # 12 tests: unit (bus, latch, palette, reset, status, tiles, sprites), oracle_pinned, spike_reference, node_addon_loads, replay_bios, replay_wizardslab
cmake --workflow --preset host-asan   # the same less the addon and the replays, under ASan and UBSan
cmake --preset pico2 && cmake --build --preset pico2

node tools/replay.mjs wizardslab bios --classes      # seven goldens, exact
PICOVDP_SPLIT_CHECK=1 node tools/replay.mjs --classes # every row built at two splits
node tools/fuzz.mjs --scope tiles --seed 1 --ops 10000000
node tools/fuzz.mjs --scope status --seed 2 --ops 10000000
PICOVDP_SPLIT_CHECK=1 node tools/fuzz.mjs --scope tiles --seed 3 --ops 10000000

# 6502-EMULATOR, on v3-vdp, after npm run build:cli
PICOVDP_ADDON=$PWD/../../C/6502-PICOVDP/host/node/Video.cjs npm run test:picovdp
```
