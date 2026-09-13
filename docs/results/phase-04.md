Phase 4 — Core: line timing, status registers, interrupts
========================================================

**Status:** done on 2026-09-13. Every check below passed. Overflow and
collision have no source until Phase 6's sprites. See
[Differences from the plan](#differences-from-the-plan), items 1 and 2.

**The headline.**

- **The raster's numbering, status and interrupts are in C**, per §3, §6, §14
  and §15. `Video.test.ts`'s block "display timing, status and interrupts"
  passes against the core, 29 of 29. So does "debugger accessors", 4 of 4.
  151 of the 342 tests pass in all, up from 116, and no test throws any more.
- **The fuzzer finds no divergence.** Two seeds of 10⁷ operations each ran in
  a new `status` scope. It compares every read, status reads included; `/INT`
  after every operation and every tick; and, every 5,000 operations, all
  sixteen status registers and the display line, on top of the bus scope's
  state. Each run made 2.9 million line starts.
- **The checks can fail.** Eight mutants of the new code were planted. Each
  was caught by `test_status`, and seven by the fuzzer. The one the fuzzer
  missed moves the sprites' once-a-frame guard, which has no source to show
  it until Phase 6.
- **Snapshots now go both ways.** A snapshot taken from the core restores into
  `Video.ts`, and the reverse. After either restore, `/INT` and all sixteen
  status registers stay in step with the original.
- **`host-asan` is clean.**

Contents: [What was built](#what-was-built) · [How a line start works](#how-a-line-start-works) ·
[Video.test.ts against the core](#videotestts-against-the-core) ·
[Unit tests](#unit-tests) · [The fuzzer](#the-fuzzer) ·
[Mutants](#mutants) · [Snapshots](#snapshots) · [Done when](#done-when) ·
[Differences from the plan](#differences-from-the-plan) ·
[For later phases](#for-later-phases) · [Reproducing](#reproducing)

---

What was built
--------------

| Part | What it is |
|---|---|
| `core/status.c` | §3, §6, §14: what happens at a line start (`vdp_raster_line_start`), the sixteen status registers as a read returns them (`vdp_status_read`) and as a debugger sees them (`vdp_status_peek`), their reset, and `vdp_int_asserted` |
| `core/vdp.h` | `vdp_t`'s bus side gains `display_line`, `stat0`, `irq_latch`, `frame_events`, `overflow_sprite` and `collision_map`. `vdp_t` is now 135,472 bytes, on the host and on the M33 |
| `core/vdp_internal.h` | the `IRQEN` and `STAT0` bits; `vdp_latch_interrupt` and `vdp_frame_event`, which Phase 6's sprites will call; `vdp_display_line_of` |
| `core/bus.c` | a status port reads through `vdp_status_read`. Phase 3's constant-only `status()` is gone |
| `core/line.c` | `vdp_line_start` runs the line's events before the render side latches; `vdp_reset` resets status |
| `core/vdp_debug.h`, `debug.c` | `vdp_debug_status` and `vdp_debug_display_line`; the snapshot carries the display line, flags, latches, the frame's spent events, `STAT7` and the collision map |
| `host/node/addon.c` | `status`, `displayLine`, and the new snapshot fields |
| `host/node/Video.cjs` | `getStatus`, `peekStatus` and `getDisplayLine` are real. `serialize`/`deserialize` carry every field `Video.ts` does. `NotInCore` is gone: nothing throws for want of a phase any more |
| `tests/unit/test_status.c` | 15 tests, 9,611 checks. `card.h`'s helpers are now `static inline`, so a test need not use them all |
| `tools/fuzz.mjs` | `--scope status` |

Nothing changed in `6502-EMULATOR`.

On the RP2350 the new hot functions are in `.time_critical`:
`vdp_raster_line_start`, `vdp_status_read`, `vdp_status_peek` and
`vdp_int_asserted`. The `pico2` build of `core/` has no warnings.

---

How a line start works
----------------------

`vdp_line_start(v, s)` is told that screen line *s* begins. Before the render
side latches, the bus side handles the line's events, in `Video.ts`'s order:

1. **The display line** is *s* less the top border of the geometry the
   registers select now. A change of picture height since the last line start
   therefore moves the numbering 24 lines here, and nowhere else (§3).
2. **A frame begins** at screen line 0, which re-arms vertical blank.
3. **Vertical blank** fires at the first line start of the frame at or past
   the end of the picture in the geometry of the moment: screen line 216 or
   240. It sets `F` whatever `IRQEN` and the display say. It latches `STAT1`
   b0 only if enabled, and it is spent for the frame either way (§14).
4. **The scanline compare** latches on every match of the display line with
   `IRQLINE`. It is not once a frame, so a line that a change of height
   repeats compares again.
5. **Overflow and collision are re-armed** as screen line 261 begins. The
   line built then is the next frame's line 0, and those events belong to the
   frame whose line is being built (§14).

`STAT1` and `/INT` are both `irq_latch & IRQEN`, taken as `IRQEN` stands at
the moment. Disabling a source therefore releases `/INT` without acknowledging
it, and enabling it again asserts `/INT` again.

A read of `STAT0` clears `F`, `OVF`, `COL` and the index field, `STAT7`, the
collision map, and the vertical blank, overflow and collision latches. A read
of `STAT1` clears every latch and nothing else (§6).

**Reset.** RST clears every flag and latch, `STAT7` and the map. It leaves the
screen line, the display line and the frame's spent events alone, so the
frame's vertical blank is not raised twice. The reset geometry numbers the line
from the next line start. A power-on also forgets the frame's events. Until the
platform's first line start, it numbers the current screen line as the reset
geometry would.

**Concurrency.** A line start and a status read are both core 1 interrupts of
the same priority on the RP2350 (PLAN.md section 3). Neither preempts the
other, so nothing here needs a lock. See [For later phases](#for-later-phases)
for the one publisher that will not be an interrupt.

---

Video.test.ts against the core
------------------------------

```
PICOVDP_ADDON=…/6502-PICOVDP/host/node/Video.cjs npm run test:picovdp
Tests:       191 failed, 151 passed, 342 total
```

| Block | Phase 3 | Phase 4 | Tests | Phase that completes it |
|---|--:|--:|--:|---|
| **display timing, status and interrupts** | 5 | **29** | **29** | 4 |
| **debugger accessors** | 2 | **4** | **4** | 4 |
| Video (TMS9918 VDP) | 41 | 47 | 48 | 5: one test draws a tile |
| direct VRAM access | 2 | 2 | 2 | 3 |
| the VDP bus | 23 | 23 | 23 | 3 |
| textGrid | 7 | 7 | 7 | — |
| the palette (§11) | 14 | 14 | 15 | 5 |
| frameIndices | 4 | 4 | 5 | 5: needs more than one colour in the frame |
| the tile engine (§8) | 5 | 6 | 108 | 5, 7 |
| sprites (§10) | 8 | 10 | 65 | 6 |
| two layers, priority and scrolling (§12, §13) | 5 | 5 | 36 | 7 |

Phase 3 reported 39 failures from accessors that threw. None throw now. All 191
remaining failures are assertions: 78 `toBe`, 112 `toEqual` and 1
`toBeGreaterThan`, on pictures and on sprite status the core does not produce
yet. The three failures outside the tile engine, sprites and layers blocks are
"should render a tile with pattern data", "agrees with the RGBA front buffer
pixel for pixel" and "renders COLOR = $1F as black on white". All three need the
tile engine.

---

Unit tests
----------

`test_status`: 15 tests, 9,611 checks.

| Test | Covers |
|---|---|
| `display_line_is_the_screen_line_less_the_top_border` | every screen line of two frames in legacy Compact, Text, Compact, Graphics, Full and a reserved `VMODE`: the display line, `STAT2`'s alias of 256–261 onto 0–5, `STAT3` b0 |
| `hblank_is_the_platforms` | `STAT3` b1 follows `vdp_set_hblank`, through a read and a peek |
| `a_change_of_height_renumbers_at_the_next_line_start` | not at the write; 24 on, and 24 back; `STAT3` b0 judges the old number against the new height |
| `vertical_blank_at_the_end_of_the_picture` | screen line 216 or 240 in four geometries, and not the line before |
| `f_sets_whatever_irqen_and_the_display_say` | `F` with display off and nothing enabled; no `STAT1` trace, and enabling afterwards raises nothing |
| `vertical_blank_once_a_frame_across_changes_of_height` | shrinking past the new end, growing after it has fired, growing across a frame's start |
| `scanline_compare_on_every_match` | the matching line and not the one before; a reprogrammed `IRQLINE`; a repeated line; a skipped line; 256–261 unmatched; disabled leaves no trace |
| `vertical_blank_and_irqline_together` | `STAT1` = `$03` at display line 192 |
| `stat1_acknowledges_the_latches_and_nothing_else` | port B's `STAT1` read leaves `STAT0`, `STAT7` and the map |
| `stat0_acknowledges_its_flags_and_their_interrupts` | `STAT7` and `STAT8`–`STAT15` read first without acknowledging; `STAT0` clears them and three latches, not the compare |
| `int_is_the_latches_still_enabled` | `MODE1` b5 and `IRQEN` b0 release and reassert without acknowledging; `IRQEN` b7:4 ignored |
| `overflow_and_collision_once_a_frame` | acknowledged at once and not raised again; the two guards independent; re-armed at screen line 261, not 260 or 0; spent while disabled stays spent |
| `each_port_reads_through_its_own_selector` | `STAT4` does not acknowledge; port B's `STAT0` read clears for both; `STATSEL` b7:4 ignored |
| `rst_clears_status_and_leaves_the_raster` | flags, latches and `/INT` clear; screen line, display line and spent events kept; renumbered at the next line start; vertical blank not raised twice; power-on forgets the frame |
| `a_snapshot_carries_status_and_the_line_as_numbered` | a display line that the registers no longer derive survives save and restore, with all sixteen registers and `/INT` |

Overflow and collision have no source before Phase 6, so their guards are
driven through `vdp_frame_event`, the call the sprites will make. Their flags
are set in `vdp_t` directly.

---

The fuzzer
----------

`--scope status` builds on the bus scope. It uses the same stream, debugger
operations included, and the same state check every 5,000 operations. It adds:

- **every read** of either port, whatever `STATSEL` names
- **`/INT` after every operation and after every tick**
- **every 5,000 operations:** all sixteen status registers, peeked, and the
  display line

**Sprites are held off.** Overflow and collision are Phase 6's, and
`Video.ts`'s sprites would otherwise raise them from zeroed VRAM within a
frame. So after any operation that leaves `Video.ts`'s `SPRCTRL` b0 set, both
cards have b0 cleared by the same debugger register write. A write or reset is
the only thing that can set it, and no tick runs before it is cleared. Nothing
else is masked: `F`, all four latches, `STAT7`, the map and `/INT` are compared
exactly. A minimised case that needed the hold is written as JSON only, since a
trace cannot carry a debugger write.

| Run | Stream | Result |
|---|---|---|
| status, seed 1, 10⁷ | 8,103,496 writes, 1,445,124 reads, 44,505 register sets, 44,683 pokes, 5,170 resets; 183,006,917 ticks, 2,876,868 line starts; sprites held off 13,393 times | no divergence, 15.4 s |
| status, seed 2, 10⁷ | 8,101,311 writes, 1,445,763 reads, 44,573 register sets, 44,777 pokes, 5,280 resets; 183,602,491 ticks, 2,886,231 line starts; sprites held off 13,305 times | no divergence, 14.5 s |
| status, `--self`, seed 1, 10⁷ | as seed 1 | no divergence, 8.7 s |
| bus, seed 1, 10⁷ (Phase 3's check, again) | as seed 1 | no divergence, 14.2 s |
| all, seed 7, 10⁵ | 80,535 writes, 14,908 reads; 39,928 line starts | diverges at operation 1,791: `STAT0` reads `$80`, `Video.ts` `$C1`, which is `F` with sprite 1 dropped. That is Phase 6's |

---

Mutants
-------

Each mutant changed one line of `core/status.c` or `vdp_internal.h`. Each was
run against `test_status` and against 10⁶ operations of `--scope status`,
seed 1.

| Mutant | `test_status` | Fuzzer |
|---|---|---|
| vertical blank not once a frame (the guard in `vdp_frame_event` removed) | 14 failed | diverges at op 5,437, minimised to 4: `STAT0` `$80`, `Video.ts` `$00` |
| vertical blank at `>` the picture's end | 30 failed | op 7,852, minimised to 3: `/INT` |
| scanline compare against the screen line | 6 failed | op 261, minimised to 8: `STAT1` `$03`, `Video.ts` `$01` |
| a `STAT0` read clears the compare latch | 3 failed | op 1,505, minimised to 21: `/INT` |
| `/INT` ignores `IRQEN` | 1 failed | op 11,102, minimised to 5: `/INT` after a `MODE1` write |
| RST forgets the frame's spent events | 3 failed | op 6,902, minimised to 4: `STAT0` `$80`, `Video.ts` `$00` |
| `STAT3` b0 by `>` | 12 failed | op 40,245, minimised to 140: `STAT3` `$00`, `Video.ts` `$01` |
| sprite guards re-armed at screen line 0, not 261 | 5 failed | **no divergence**: nothing raises overflow or collision until Phase 6 |

The sources were restored afterwards, and `test_status` passed again on
them.

---

Snapshots
---------

The snapshot now holds everything `Video.ts`'s does except the frame buffers,
which neither format carries:

- the display line, stored rather than derived. After a mode change and before
  the next line start, the registers no longer give it.
- `STAT0`, the latches, the frame's spent events, `STAT7` and the collision
  map

Phase 3's reverse restore, from the core into `Video.ts`, failed for want of
these fields. It works now. A scratch check ran both cards with `IRQEN` b0 set,
`IRQLINE` = 100 and 1.5 frames of ticks, then wrote `VMODE` after the last line
start. It serialized each card and restored it into the other kind. It compared
the ten serialized fields both formats share. It then ticked all four cards for
40,000 cycles, comparing `/INT` every cycle and all sixteen status registers
every 997. Nothing differed.

---

Done when
---------

| Check | Result |
|---|---|
| the Jest block for timing, status and interrupts passes against the core | ✅ 29/29; debugger accessors 4/4 too |
| the fuzzer, now with line starts and status reads, runs 10⁷ operations with no divergence in reads or in `/INT` after any operation | ✅ `--scope status`, seeds 1 and 2, with sprites held off (item 1 below) |
| the host suite passes | ✅ `cmake --workflow --preset host`: 8 of 8 |
| `host-asan` is clean | ✅ `cmake --workflow --preset host-asan`: 7 of 7 under ASan and UBSan |
| firmware presets still build | ✅ `pico2`, `core/` with no warnings |

---

Differences from the plan
-------------------------

1. **The fuzzer holds sprites off.** The done-when asks for no divergence in
   reads or `/INT`. `Video.ts`'s sprites raise overflow and collision, which
   are Phase 6's, so the status scope keeps `SPRCTRL` b0 clear on both cards
   (see [The fuzzer](#the-fuzzer)). Phase 6's done-when now also requires
   the status scope to run with sprites on.
2. **The overflow and collision guards are built but have no source.** The
   plan lists "the once-a-frame guards for vblank, overflow and collision" here.
   All three are in `vdp_frame_event` and re-armed at the right line starts.
   The sprites that trigger two of them are Phase 6's, so those two are proven
   by `test_status` alone for now. The same holds for `STAT7` and the collision
   map: they are read and cleared, but nothing fills them yet.
3. **`vdp_debug.h` gains `vdp_debug_status` and `vdp_debug_display_line`.**
   The snapshot gains every status field and the display line.
4. **`vdp_int_asserted` and the status reads are `VDP_HOT`.** The bus
   interrupt calls them.
5. **The adapter's `NotInCore` is removed.** It named the phase an accessor was
   waiting for, and no accessor waits any more.
6. **No 6502-EMULATOR changes.**

---

For later phases
----------------

- **Phase 6: collision is published from the build.** Everything this phase
  publishes happens at a line start, which is an interrupt on the RP2350.
  Section 3 has core 1 publish `COL`, the collision latch and the map as it
  merges the sprite line. That runs in thread mode, where a bus interrupt can
  land in the middle of `stat0 |= COL` and acknowledge a `STAT0` the update then
  overwrites. The core's host tests cannot see this. Phase 6 should publish
  collision so that a status read cannot interleave: masked briefly, or
  through a pending word the interrupt folds in. Phase 8 should measure the
  choice.
- **Phase 11: restaging.** Section 3 says the latch restages the PIO's four
  bytes whenever status changes. The line start is the only place status
  changes without a read, so `vdp_line_start` is where Phase 11 hooks in.

---

Reproducing
-----------

```sh
cmake --workflow --preset host        # 8 tests: unit (bus, latch, palette, reset, status), oracle_pinned, spike_reference, node_addon_loads
cmake --workflow --preset host-asan   # the same less the addon, under ASan and UBSan
cmake --preset pico2 && cmake --build --preset pico2

node tools/fuzz.mjs --scope status --seed 1 --ops 10000000
node tools/fuzz.mjs --scope status --seed 2 --ops 10000000
node tools/fuzz.mjs --self --scope status --ops 10000000
node tools/fuzz.mjs --scope bus --seed 1 --ops 10000000

# 6502-EMULATOR, on v3-vdp, after npm run build:cli
PICOVDP_ADDON=$PWD/../../C/6502-PICOVDP/host/node/Video.cjs npm run test:picovdp
```
