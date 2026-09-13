Phase 1 — Timing spike on the Pico 2
====================================

**Status:** done on 2026-09-13. Every row of §18's table is measured at both
clocks, Still Open 2 is answered, and the clock preset is chosen: **352 MHz**.

**The headline:** §18 underestimates the line by a factor of 2.5 to 4.5. The
Full mode worst case does not fit the line at either clock. Its margin is −25%
at 302.4 MHz and −7% at 352 MHz with 32 plain 16 × 16 sprites, and −85% and
−59% with them magnified and detailed collision on. That is far below the 25%
this phase was to check for, so Still Open 1's remedies are weighed below. **A
decision is needed before Phase 3 begins.** Two layers with no sprites fit
comfortably: 40% margin at 352 MHz.

Contents: [The spike](#the-spike) · [Method](#method) · [§18, measured](#18-measured) ·
[Whole lines](#whole-lines) · [Bit depths](#bit-depths) · [Still Open 2](#still-open-2--the-4bpp-table) ·
[Clock preset](#clock-preset) · [Still Open 1](#still-open-1--the-remedies-weighed) ·
[Why §18 was wrong](#why-18-was-wrong) · [Done when](#done-when) · [Differences from the plan](#differences-from-the-plan) ·
[Reproducing](#reproducing)

---

The spike
---------

`spike/`, disposable, as the plan says. Its parts:

| File | What it is |
|---|---|
| `render.c`, `render.h` | Portable C11 scanline renderer. Covers §8 at all four depths with per-cell attributes, flips, priority and pattern bit 8; §13's 9-bit scroll; §10's evaluation, 16 × 16 quadrants, magnification, flips, 9-bit X, negative Y, limit, overflow, plain and detailed collision; §12's seven levels; §11's paired-cache expansion. Graphics and Full geometries only. No bus, no legacy submode, no status registers |
| `scenes.c` | The worst-case scenes (below) |
| `pico_main.c` | RP2350 harness. Core 1 times each stage with the DWT cycle counter, interrupts off; core 0 owns USB and switches the clock |
| `host_main.c` | Host check under CTest (`spike_reference`), described below |
| `capture.mjs`, `report.mjs` | Run the suite on the board over USB CDC, and turn the capture into these tables |

**The timed code draws what SPEC.md says.** The host check compares every
worst-case scene over 9 frames, and 400 random scenes over 2, pixel for pixel
against a deliberately naive per-pixel reference. The reference is written
straight from §8, §10, §12 and §13, and the check covers the sticky status
too. The random scenes put sprites off every edge, entering from the top and
left, at 8 × 8, crowded against the right edge, over sparse patterns and random
layer settings.

The check was itself tested by planting bugs in the final renderer, one at a
time: the column wrap, clipping at both edges, every flip, the priority
classes, the claim bitmap, detailed collision, magnification, the limit and the
terminator among them. All 26 were caught. The right-edge clip needed the
crowded scenes first: without them, a collision off the picture hid behind a
real one. The check runs clean under ASan and UBSan. So the numbers below are
for a renderer that does the real work, not one that skips some.

**The worst-case scenes.** Each is the worst a line can be at its settings:

- both layers on, per-cell attributes, scrolled, with the scroll moving every
  frame so every cell offset is hit
- layer 0 opaque; layer 1 half transparent, pixel by pixel
- a quarter of the cells with the priority bit, and random flips and bit 8
- `SPRCOUNT` 64 with all 64 slots covering the band, so evaluation fills the
  list and overflows
- `SPRLIMIT` sprites drawn, every sprite pixel solid, spread across the
  picture with each overlapping the next, so every pair collides

Scenes are named `geometry-depth-sprites[-det][-notab][-limN][-l0only]`:
sprites `16` are 16 × 16, `32` are 16 × 16 magnified.

**Two passes.** The first renderer drew a pixel at a time and measured 20 to
50 cycles a pixel (`phase-01-pass1.txt`). Numbers from code that naive would
have argued for remedies the design doesn't need, so it was rewritten once.
The second pass works in 32-bit words:

- **Layers.** A cell's eight pixels are two words of indices and two of solid
  masks, merged by masks against a per-pixel priority class.
- **Sprites.** They claim pixels in a 320-bit bitmap. Collision is a word AND.
  Each sprite's row is decoded once, and only the pixels it newly claims are
  drawn, four at a time.

That halved everything:

| Scene | Stage | First pass | Final |
|---|---|--:|--:|
| full-4bpp-16 | eval | 1,256 | 1,258 |
| full-4bpp-16 | l0 | 6,556 | 2,848 |
| full-4bpp-16 | l1 | 10,455 | 5,019 |
| full-4bpp-16 | spr | 21,623 | 9,419 |
| full-4bpp-16 | exp | 2,400 | 1,504 |
| full-4bpp-16 | total | 42,220 | 19,977 |
| full-4bpp-32-det | eval | 1,256 | 1,258 |
| full-4bpp-32-det | l0 | 6,556 | 2,848 |
| full-4bpp-32-det | l1 | 10,455 | 5,019 |
| full-4bpp-32-det | spr | 51,939 | 20,987 |
| full-4bpp-32-det | exp | 2,400 | 1,504 |
| full-4bpp-32-det | total | 72,512 | 31,568 |

More is possible, magnified sprites and detailed collision especially (see
[Why §18 was wrong](#why-18-was-wrong)), but not another factor of two.

Method
------

- **Board:** Pico 2, RP2350 A2, the same silicon as the PRO's RP2354A.
  `copy_to_ram`, everything in SRAM; the renderer's symbols are at `0x2000xxxx`.
- **Clocks:** pico9918's VGA presets 1 and 2. 302.4 MHz is PLL 1512 MHz ÷ 5 at
  1.20 V; 352 MHz is 1056 MHz ÷ 3 at 1.30 V. Both ran the whole suite with no
  fault. 252 MHz was not used.
- **Compiler:** Arm GNU Toolchain 15.3.Rel1. The `pico2` preset's `-O2`
  (RelWithDebInfo) for every figure here. `-O3` was also measured
  (`phase-01-O3.txt`): 1–4% faster, not a remedy.
- **Timing:** DWT `CYCCNT` on core 1, read around each stage of every line.
  Reading it costs 1 cycle. Every figure is the **maximum** over 20 frames ×
  240 lines, and the maxima fall in the sprite band, as intended. Band means are
  within 8% of the maxima.
- **Two cross-checks.**
  - An uninstrumented pass timed both by cycles and by the µs timer agrees to
    0.0008% in every scene.
  - The same code counts exactly the same cycles at both clocks: 0.00%
    difference in every scene.

  So a cycle count is a clock-independent cost. Only the budget changes.
- **Budget:** a display line is 63.556 µs. That is **19,219 cycles** at
  302.4 MHz and **22,371** at 352 MHz.
- **Stand-ins** for what has no code yet, shaped like section 3 of the plan:
  - *Bus interrupt service.* A spare IRQ pended on core 1, entry and exit
    included. Its handler reads a PIO FIFO register, decodes, writes VRAM and
    the journal, advances the pointer, and restages a four-byte word to a PIO TX
    FIFO. Priced at 32 accesses per display line: back-to-back `sta abs` at
    2 MHz, as §18 assumes.
  - *Line start.* The register snapshot, plus 32 journal entries drained, all in
    the palette window.

  Phase 11 and Phase 3 replace these with the real thing.

§18, measured
-------------

Cycles. The §18 column is its estimate; the rows are its rows.

| Work | §18 estimate | Graphics, 256 px | Full, 320 px |
|---|--:|--:|--:|
| Layer 0, 4bpp | ~800 | 2,313 | 2,848 |
| Layer 1, 4bpp with transparency merge | ~1,600 | 4,069 | 5,019 |
| Sprite evaluation, 64 slots | ~500 | 1,258 | 1,258 |
| Sprite composite, 32 × 16 px | ~2,600 | 8,925 | 9,419 |
| Border and palette expansion, 320 px | ~1,300 | 1,617 | 1,504 |
| Bus interrupt service, 32 accesses (2 MHz, one display line) | ~1,200 | 3,136 | 3,136 |
| **Total** | **~8,000** | **21,262** | **23,113** |
| *plus* detailed collision | *~500* | *4,542* | *4,044* |
| *plus* magnified sprites | *~2,600* | *6,162* | *6,162* |
| Not in §18: line start, 32 journal entries all in the palette window | — | 912 | 912 |

The same figures as a share of the line, at each clock:

| Work, Full mode | Cycles | % of line @ 302.4 MHz | % of line @ 352 MHz |
|---|--:|--:|--:|
| Layer 0 | 2,848 | 15% | 13% |
| Layer 1 | 5,019 | 26% | 22% |
| Sprite evaluation | 1,258 | 7% | 6% |
| Sprite composite, 32 × 16 | 9,419 | 49% | 42% |
| Expansion | 1,504 | 8% | 7% |
| Bus service | 3,136 | 16% | 14% |
| Line start | 912 | 5% | 4% |
| **Line, worst, 16 × 16** | **24,025** | **125%** | **107%** |
| Detailed collision, added | 4,044 | 21% | 18% |
| Magnification, added | 6,162 | 32% | 28% |

The bus stand-in, per access:

| Bus interrupt stand-in | Max | Mean |
|---|--:|--:|
| data-write | 98 | 95 |
| data-read | 84 | 83 |
| reg-write | 93 | 93 |
| status-read | 83 | 82 |

Whole lines
-----------

The build, plus bus service and line start (4,048 cycles), against the line:

| Scene | Build | + bus and line start | Margin @ 302.4 | Margin @ 352 |
|---|--:|--:|--:|--:|
| graphics-4bpp-16 | 18,126 | 22,174 | −15% | 1% |
| graphics-4bpp-16-det | 22,677 | 26,725 | −39% | −19% |
| graphics-4bpp-32 | 24,289 | 28,337 | −47% | −27% |
| graphics-4bpp-32-det | 30,324 | 34,372 | −79% | −54% |
| full-4bpp-16 | 19,977 | 24,025 | −25% | −7% |
| full-4bpp-16-det | 24,036 | 28,084 | −46% | −26% |
| full-4bpp-32 | 26,165 | 30,213 | −57% | −35% |
| full-4bpp-32-det | 31,568 | 35,616 | −85% | −59% |

Without sprites the line is fine. Full mode, 4bpp, the sum of the stage maxima:

- **Two layers:** 9,371 cycles, 13,419 with bus and line start. Margin 30% at
  302.4 MHz, 40% at 352 MHz.
- **One layer:** 4,387 cycles, 8,435 with bus and line start. Margin 56% and
  62%.

Sprites are what don't fit.

Bit depths
----------

Cycles; the last column is the whole line with magnified sprites and detailed
collision, bus and line start included.

| Depth | Geometry | Layer 0 | Layer 1 | Sprites 16 | Sprites 16 det | Sprites 32 | Sprites 32 det | Line, worst (32 det) |
|---|---|--:|--:|--:|--:|--:|--:|--:|
| 1bpp | graphics | 1,828 | 3,564 | 7,797 | 12,946 | 14,406 | 20,885 | 33,188 |
| 2bpp | graphics | 2,273 | 3,991 | 8,121 | 12,762 | 14,795 | 20,368 | 33,488 |
| 4bpp | graphics | 2,313 | 4,069 | 8,925 | 13,467 | 15,087 | 21,110 | 34,372 |
| 8bpp | graphics | 1,690 | 3,100 | 8,337 | 12,668 | 14,598 | 20,801 | 32,481 |
| 1bpp | full | 2,248 | 4,390 | 8,288 | 12,936 | 14,897 | 20,747 | 34,182 |
| 2bpp | full | 2,787 | 4,919 | 8,570 | 12,830 | 15,244 | 20,332 | 34,769 |
| 4bpp | full | 2,848 | 5,019 | 9,419 | 13,463 | 15,581 | 20,987 | 35,616 |
| 8bpp | full | 2,070 | 3,817 | 8,833 | 12,677 | 15,094 | 20,682 | 33,342 |

§18 has the order right: 8bpp is the cheapest layer, 4bpp the dearest. The
spread is smaller than it implies, about 30% from cheapest to dearest, because
most of a cell's cost is the name, attribute and pattern fetches and the
priority merge, not the unpacking. Sprite cost barely depends on depth.

Still Open 2 — the 4bpp table
-----------------------------

| 4bpp layers | Graphics L0 | Graphics L1 | Full L0 | Full L1 |
|---|--:|--:|--:|--:|
| Table (8 KB) | 2,313 | 4,069 | 2,848 | 5,019 |
| Arithmetic | 2,744 | 4,253 | 3,380 | 5,247 |

The comparison is against the best arithmetic path the spike found, not a naive
one: one 32-bit load for the pattern row, and the eight nibbles spread to bytes
with shifts and a carry-free non-zero test.

**Answer: yes, it earns its place, by less than §18 assumed.** It saves 16% on
an opaque layer and 4–5% on a merged one: 760 cycles on a Full two-layer line.
8 KB is 1.5% of the SRAM. Keep it, and build it first, as §18 says. It does not
make 4bpp "about 2 cycles a pixel": the table-driven opaque layer is 8.9 cycles
a pixel in Full mode.

Clock preset
------------

**352 MHz** (PLL 1056 MHz ÷ 3, VREG 1.30 V). It gives 16% more cycles per line
than 302.4 MHz, and on these figures every one is needed. It is the first of
Still Open 1's remedies, and changes nothing SPEC.md specifies (§2 already lists
it).

What it costs is what Phases 8, 10 and 13 check: stability and temperature at
1.30 V over their ten- and thirty-minute runs. Neither preset showed a fault
here, but this suite runs for about a minute a clock. The flash SPI divider is 4,
as pico9918 sets it, so a firmware that ever touches flash at 352 MHz stays in
range.

Still Open 1 — the remedies weighed
-----------------------------------

The trigger: the Full mode worst case leaves −25% of the line at 302.4 MHz
against the 25% the plan requires. So, in Still Open 1's order, each measured on
Full mode, 4bpp, two layers, with bus service and line start included:

| Full mode, 4bpp | Sprites | Layers | SPRLIMIT | Line, worst | Margin @ 302.4 | Margin @ 352 |
|---|---|---|--:|--:|--:|--:|
| full-4bpp-16 | 16 × 16 | two | 32 | 24,025 | −25% | −7% |
| full-4bpp-16-lim24 | 16 × 16 | two | 24 | 22,038 | −15% | 1% |
| full-4bpp-16-lim16 | 16 × 16 | two | 16 | 19,611 | −2% | 12% |
| full-4bpp-16-lim8 | 16 × 16 | two | 8 | 17,230 | 10% | 23% |
| full-4bpp-16-l0only | 16 × 16 | one | 32 | 19,093 | 1% | 15% |
| full-4bpp-16-lim16-l0only | 16 × 16 | one | 16 | 14,662 | 24% | 34% |
| full-4bpp-16-det | 16 × 16, detailed | two | 32 | 28,084 | −46% | −26% |
| full-4bpp-16-det-lim24 | 16 × 16, detailed | two | 24 | 24,748 | −29% | −11% |
| full-4bpp-16-det-lim16 | 16 × 16, detailed | two | 16 | 21,380 | −11% | 4% |
| full-4bpp-16-det-lim8 | 16 × 16, detailed | two | 8 | 18,099 | 6% | 19% |
| full-4bpp-16-det-l0only | 16 × 16, detailed | one | 32 | 23,126 | −20% | −3% |
| full-4bpp-16-det-lim16-l0only | 16 × 16, detailed | one | 16 | 16,433 | 14% | 27% |
| full-4bpp-32 | magnified | two | 32 | 30,213 | −57% | −35% |
| full-4bpp-32-lim24 | magnified | two | 24 | 26,123 | −36% | −17% |
| full-4bpp-32-lim16 | magnified | two | 16 | 22,956 | −19% | −3% |
| full-4bpp-32-lim8 | magnified | two | 8 | 19,248 | 0% | 14% |
| full-4bpp-32-l0only | magnified | one | 32 | 25,263 | −31% | −13% |
| full-4bpp-32-lim16-l0only | magnified | one | 16 | 18,006 | 6% | 20% |
| full-4bpp-32-det | magnified, detailed | two | 32 | 35,616 | −85% | −59% |
| full-4bpp-32-det-lim24 | magnified, detailed | two | 24 | 29,508 | −54% | −32% |
| full-4bpp-32-det-lim16 | magnified, detailed | two | 16 | 25,010 | −30% | −12% |
| full-4bpp-32-det-lim8 | magnified, detailed | two | 8 | 20,128 | −5% | 10% |
| full-4bpp-32-det-l0only | magnified, detailed | one | 32 | 30,667 | −60% | −37% |
| full-4bpp-32-det-lim16-l0only | magnified, detailed | one | 16 | 20,066 | −4% | 10% |

1. **352 MHz.** Adopted. Alone it is not enough: −7% with plain 16 × 16 sprites
   and −59% at worst.
2. **A lower default `SPRLIMIT`.** At 352 MHz, 16 reaches 12% for plain
   sprites and −12% for magnified with detailed collision; 8 reaches 23% and
   10%. Nothing on the list reaches 25% for every sprite setting. The catch is
   that a default is only a default: software may set 32 and get late lines, so
   choosing this also means specifying what a late line does.
3. **Full mode as a single-layer mode.** At 352 MHz: 15% for plain sprites, −37%
   at worst. Worth less than halving `SPRLIMIT` on its own terms. It is also the
   one remedy that changes behaviour (risk 1), and **it breaks the emulator's
   `vdp-layers` fixture**, which scrolls two 4bpp layers in Full mode.

**The list alone doesn't close the gap.** Its best combination, 352 MHz with
`SPRLIMIT` 16 and a single layer, still leaves 10% at worst and gives up two
features. Two options the list doesn't have belong in the weighing:

- **Build the sprites on core 0, in parallel.** Core 0's work in section 3 is
  the VGA DMA interrupt, plus USB and statistics in debug builds. Split the line
  build between the cores:
  - core 1 drains the journal at the latch, then builds the layers while core 0
    builds the sprites from the same stable render copy
  - core 1 merges the sprite line (one SWAR pass, estimated at ~1,000 cycles)
    and expands

  Core 1's line becomes eval + layers + merge + expansion + bus + line start,
  about 15,700 cycles: **30% margin at 352 MHz whatever the sprite settings.**
  Core 0 carries 9,400 to 21,000 cycles of sprites, 58% to 6% of its line,
  alongside the DMA interrupt. SPEC.md doesn't change. Section 3 of the plan
  does: the core split, and publishing `OVF`, `COL` and the collision map from
  core 0. It needs a short follow-up spike to measure core 0 under a simulated
  DMA interrupt and the cross-core handoff before Phase 3 designs around it.
- **Say what a late line does, whatever else is chosen.** The plan already
  counts late lines, and pico9918 silently drops them. SPEC.md is silent. If the
  worst case is ever allowed to overrun, §18 should say what the monitor shows,
  such as the previous line repeated, and that nothing else (status, interrupts,
  VRAM) is affected.

**Recommendation.** Keep 352 MHz. Spike the core 0 sprite split next, before
Phase 3, since it is the only option that keeps SPEC.md's feature set within
budget. If it doesn't hold up, fall back to `SPRLIMIT` 16 by default with a
specified late-line behaviour. Don't make Full a single-layer mode. §18 and Still
Open 1 and 2 in SPEC.md, all three copies (ground rules 2 and 7), are then
rewritten once, with the chosen remedy, rather than twice.

Why §18 was wrong
-----------------

- **Cycles per instruction.** §18 counted instructions and priced them at a
  cycle each. On this M33 in SRAM, loads and stores cost about two. The
  expansion loop is five instructions a pixel (a byte load, a word load, a word
  store, compare, branch) and measures 7.5 cycles. Sprite evaluation, which
  touches memory once a slot, measures one cycle per instruction. Table-driven
  code pays for every table load.
- **Instruction counts.** A cell is not a few loads and stores. Each one needs
  its name, attribute and pattern fetched with 64 KB wrap, the flips applied,
  and the priority class written beside the indices so later sources can
  compete (§12). That runs to 60–70 instructions a cell in the merge path.
- **Detailed collision** was priced as an owner index in the line buffer,
  ~500 cycles. Finding every colliding pair exactly costs 4,000–4,500: an
  owner list per 32-pixel word, scanned for every sprite that overlaps. A
  cheaper exact method may exist; an approximate one would break §10.
- **Magnification** doubles each sprite's pixel work, as §18 says. On top of
  that come expanding the decoded row (16 bytes to 32) and twice the draw words:
  6,200 against §18's 2,600.
- **Bus service** is ~95 cycles an access against §18's ~38, and exception
  entry and exit are most of it. The real handler's MMIO may cost more or less.
  Phase 11 measures it.

Done when
---------

| Check | Result |
|---|---|
| Every row of §18's table has a measured figure at both clocks | ✅ [§18, measured](#18-measured). Cycle counts are identical at both clocks; margins are given for each |
| Still Open 2 has an answer | ✅ Yes: 16% on an opaque layer, 4–5% merged; keep the table |
| A clock preset is chosen | ✅ 352 MHz |
| If the Full mode worst case leaves < 25% at 302.4 MHz, Still Open 1's remedies are weighed, in order, before Phase 3 | Triggered (−25%). Weighed [above](#still-open-1--the-remedies-weighed); **the choice is the owner's and gates Phase 3** |
| Host suite green | ✅ `host` and `host-asan`: `test_placeholder`, `spike_reference` |

Differences from the plan
-------------------------

- **The renderer had two passes.** The first pass's figures are kept
  (`phase-01-pass1.txt`) and shown above, but they don't stand for the budget.
- **A host check was added:** `spike_reference` under CTest, which the plan
  didn't ask for. A spike whose output is never compared could be fast because
  it is wrong.
- **Bus service and line start are stand-ins.** The phase has no bus; the
  shapes follow section 3.
- **The worst case is harsher than §18's composite row:** 64 covering slots,
  every sprite pixel solid, every neighbour overlapping, and 32 bus accesses on
  the same line. §18 describes the same line but prices less of it.
- **Extra scenes** measure the remedies: `SPRLIMIT` 8, 16, 24 and layer 1 off.
- **`-O3` was also measured**, for comparison.
- **SPEC.md is not yet updated** with these figures (ground rule 7). §18's
  table and Still Open 1 would be rewritten again by whichever remedy is chosen,
  so the three copies are changed once, with that decision.

Reproducing
-----------

```sh
cmake --workflow --preset host                   # spike_reference: the renderer against the reference
cmake --preset pico2 && cmake --build --preset pico2
picotool load -x -f build/pico2/spike/picovdp_spike.uf2
node spike/capture.mjs docs/results/phase-01-final.txt
node spike/report.mjs docs/results/phase-01-final.txt docs/results/phase-01-pass1.txt
```

`-DPICOVDP_SPIKE_OPT=-O3` on the configure builds the `-O3` variant. The suite
takes about two minutes. The Pico 2 is left running the spike image; Phase 8
replaces it.

| File | Holds |
|---|---|
| `phase-01-final.txt` | this pass, `-O2`, 60 scenes at both clocks: the figures above |
| `phase-01-O3.txt` | this pass before the remedy scenes were added, `-O3`, 40 scenes |
| `phase-01-pass1.txt` | the first, per-pixel renderer, `-O2`, 40 scenes |
