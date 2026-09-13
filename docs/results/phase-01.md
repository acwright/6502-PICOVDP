Phase 1 — Timing spike on the Pico 2
====================================

**Status:** done on 2026-09-13. Every row of §18's table is measured at both
clocks, Still Open 2 is answered, and the clock preset is chosen: **352 MHz**.
Still Open 1's remedies are weighed, including one the list didn't have:
building sprites on core 0. It was spiked and measured.

**The headline.**

- **One core doesn't fit.** §18 underestimates the line by a factor of 2.5 to
  4. On one core, the Full mode worst case doesn't fit at either clock. With the
  real interrupt load at 352 MHz, a line takes 26,400 to 33,500 cycles against a
  budget of 22,371.
- **Sprites on core 0 fit.** Split across the cores, with the boundary chosen
  per line, **all 32 worst-case scenes fit at 352 MHz with no late lines**. The
  margin runs from 28% down to 2%, lowest for Full mode, 4bpp, magnified sprites
  with detailed collision.
- **The 25% margin needs a lower limit too.** The plan asks for 25%. In Full
  mode that takes the split *and* `SPRLIMIT` 16: margins of 25–26% for every
  sprite setting.

**Decided:** the split is adopted, with `SPRLIMIT` reset to 16 and late lines
specified — SPEC.md draft 0.4, in all three copies, with the emulator on
`v3-vdp` implementing it
([below](#still-open-1--the-remedies-weighed)).

Contents: [The spike](#the-spike) · [Method](#method) · [§18, measured](#18-measured) ·
[Whole lines on one core](#whole-lines-on-one-core) · [Bit depths](#bit-depths) ·
[Still Open 2](#still-open-2--the-4bpp-table) · [Clock preset](#clock-preset) ·
[Sprites on core 0](#sprites-on-core-0) · [Still Open 1](#still-open-1--the-remedies-weighed) ·
[Why §18 was wrong](#why-18-was-wrong) · [Done when](#done-when) ·
[Differences from the plan](#differences-from-the-plan) · [Reproducing](#reproducing)

---

The spike
---------

`spike/`, disposable, as the plan says. Its parts:

| File | What it is |
|---|---|
| `render.c`, `render.h` | Portable C11 scanline renderer. Covers §8 at all four depths with per-cell attributes, flips, priority and pattern bit 8; §13's 9-bit scroll; §10's evaluation, 16 × 16 quadrants, magnification, flips, 9-bit X, negative Y, limit, overflow, plain and detailed collision; §12's seven levels; §11's paired-cache expansion. It can build the sprites on a second core and merge them. Graphics and Full geometries only. No bus, no legacy submode, no status registers |
| `scenes.c` | The worst-case scenes (below) |
| `pico_main.c` | RP2350 harness. `r` times each stage on core 1 with the DWT cycle counter, interrupts off. `s` runs the split across both cores with real interrupt load and times each line end to end. Core 0 owns USB and the clock |
| `host_main.c` | Host check under CTest (`spike_reference`), described below |
| `capture.mjs`, `report.mjs` | Run a suite on the board over USB CDC, and turn the capture into these tables |

**The timed code draws what SPEC.md says.** The host check builds every
worst-case scene over 9 frames, and 400 random scenes over 2, three ways:

- on one core
- with all sprites split off
- split at the middle of the picture

Each is compared pixel for pixel against a deliberately naive per-pixel
reference, written straight from §8, §10, §12 and §13, and the check covers the
sticky status too. The random scenes put sprites off every edge, entering from
the top and left, at 8 × 8, crowded against the right edge, over sparse patterns
and random layer settings.

The check was itself tested by planting bugs in the final renderer, one at a
time. They covered the column wrap, clipping at both edges and at the split
boundary, every flip, the priority classes, the claim bitmap, detailed collision,
magnification, the merge, the limit and the terminator, among others. All were
caught except two equivalent mutants: `>` for `>=` where the levels can never
be equal, and a wider hit mask that marks only sprites already marked. The
right-edge clip needed the crowded scenes first: without them, a collision off
the picture hid behind a real one. The check runs clean under ASan and UBSan.
So the numbers below are for a renderer that does the real work, not one that
skips some.

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

**The renderer went through several passes.** The first drew a pixel at a time,
at 20 to 50 cycles a pixel (`phase-01-pass1.txt`). Numbers from code that naive
would have argued for remedies the design doesn't need. The final renderer
works in 32-bit words:

- **Layers.** A cell's eight pixels are two words of indices and two of solid
  masks, merged by masks against a per-pixel priority class.
- **Sprites.** They claim pixels in a 320-bit bitmap, so collision is a word
  AND. Each sprite's row is decoded once, magnified by word arithmetic, and only
  the pixels it newly claims are drawn, four at a time.
- **Detailed collision.** Settled once per line from an accumulated hit mask.

| Scene | Stage | First pass | Final |
|---|---|--:|--:|
| full-4bpp-16 | eval | 1,256 | 1,258 |
| full-4bpp-16 | l0 | 6,556 | 2,846 |
| full-4bpp-16 | l1 | 10,455 | 5,021 |
| full-4bpp-16 | spr | 21,623 | 10,087 |
| full-4bpp-16 | exp | 2,400 | 1,662 |
| full-4bpp-16 | total | 42,220 | 20,828 |
| full-4bpp-32-det | eval | 1,256 | 1,258 |
| full-4bpp-32-det | l0 | 6,556 | 2,846 |
| full-4bpp-32-det | l1 | 10,455 | 5,021 |
| full-4bpp-32-det | spr | 51,939 | 15,850 |
| full-4bpp-32-det | exp | 2,400 | 1,667 |
| full-4bpp-32-det | total | 72,512 | 26,596 |

Method
------

- **Board:** Pico 2, RP2350 A2, the same silicon as the PRO's RP2354A.
  `copy_to_ram`, everything in SRAM; the renderer's symbols are at `0x2000xxxx`.
- **Clocks:** pico9918's VGA presets 1 and 2. 302.4 MHz is PLL 1512 MHz ÷ 5 at
  1.20 V; 352 MHz is 1056 MHz ÷ 3 at 1.30 V. No fault at either. 252 MHz was not
  used.
- **Compiler:** Arm GNU Toolchain 15.3.Rel1. The `pico2` preset's `-O2`
  (RelWithDebInfo) for every figure here. `-O3` (`phase-01-O3.txt`) ranges from
  1% slower to 2.3% faster: not a remedy.
- **Stage timing** (`r`, `phase-01-final.txt`): DWT `CYCCNT` on core 1, read
  around each stage of every line, with interrupts off. Reading it costs 1 cycle.
  Every figure is the **maximum** over 20 frames × 240 lines, and the maxima fall
  in the sprite band, as intended. Two cross-checks:
  - An uninstrumented pass timed both by cycles and by the µs timer agrees to
    0.001% in every scene.
  - The same code counts exactly the same cycles at both clocks.

  So a cycle count is a clock-independent cost. Only the budget changes.
- **Budget:** a display line is 63.556 µs. That is **19,219 cycles** at
  302.4 MHz and **22,371** at 352 MHz.
- **Stand-ins in the stage suite,** shaped like section 3 of the plan:
  - *Bus interrupt service.* A spare IRQ pended on core 1, entry and exit
    included. Its handler reads a PIO FIFO register, decodes, writes VRAM and
    the journal, advances the pointer, and restages a four-byte word. Priced at
    32 accesses a display line: back-to-back `sta abs` at 2 MHz, as §18 assumes.
  - *Line start.* The register snapshot, plus 32 journal entries drained, all in
    the palette window.
- **Line timing with real interrupts** (`s`, `phase-01-split.txt`, 352 MHz). The
  interrupts run for real rather than being added up:
  - **Bus:** a PWM interrupt on core 1 every **2 µs**, without pause, running
    the bus stand-in (a data write into the palette window).
  - **VGA:** a PWM interrupt on core 0 at the VGA line rate, shaped like
    pico9918's `dmaIrqHandler` (at most 36 cycles).
  - **Line start:** each line's latch drains the journal the bus interrupt
    filled, with interrupts masked, as the latch will be.

  A line is timed on core 1 from its latch to its last expanded pixel, waiting
  for core 0 included. A line over budget is counted as late.

§18, measured
-------------

Cycles, one core, interrupts off, bus service added from the stand-in. The §18
column is its estimate; the rows are its rows.

| Work | §18 estimate | Graphics, 256 px | Full, 320 px |
|---|--:|--:|--:|
| Layer 0, 4bpp | ~800 | 2,312 | 2,846 |
| Layer 1, 4bpp with transparency merge | ~1,600 | 4,072 | 5,021 |
| Sprite evaluation, 64 slots | ~500 | 1,258 | 1,258 |
| Sprite composite, 32 × 16 px | ~2,600 | 9,592 | 10,087 |
| Border and palette expansion, 320 px | ~1,300 | 1,777 | 1,662 |
| Bus interrupt service, 32 accesses (2 MHz, one display line) | ~1,200 | 3,040 | 3,040 |
| **Total** | **~8,000** | **21,997** | **23,868** |
| *plus* detailed collision | *~500* | *2,490* | *2,530* |
| *plus* magnified sprites | *~2,600* | *3,185* | *3,185* |
| Not in §18: line start, 32 journal entries all in the palette window | — | 866 | 866 |

The same figures as a share of the line, at each clock:

| Work, Full mode | Cycles | % of line @ 302.4 MHz | % of line @ 352 MHz |
|---|--:|--:|--:|
| Layer 0 | 2,846 | 15% | 13% |
| Layer 1 | 5,021 | 26% | 22% |
| Sprite evaluation | 1,258 | 7% | 6% |
| Sprite composite, 32 × 16 | 10,087 | 52% | 45% |
| Expansion | 1,662 | 9% | 7% |
| Bus service | 3,040 | 16% | 14% |
| Line start | 866 | 5% | 4% |
| **Line, worst, 16 × 16** | **24,734** | **129%** | **111%** |
| Detailed collision, added | 2,530 | 13% | 11% |
| Magnification, added | 3,185 | 17% | 14% |

The bus stand-in, per access:

| Bus interrupt stand-in | Max | Mean |
|---|--:|--:|
| data-write | 95 | 93 |
| data-read | 84 | 83 |
| reg-write | 93 | 93 |
| status-read | 81 | 80 |

Whole lines on one core
-----------------------

The build, plus bus service and line start (3,906 cycles):

| Scene | Build | + bus and line start | Margin @ 302.4 | Margin @ 352 |
|---|--:|--:|--:|--:|
| graphics-4bpp-16 | 18,957 | 22,863 | −19% | −2% |
| graphics-4bpp-16-det | 21,447 | 25,353 | −32% | −13% |
| graphics-4bpp-32 | 22,153 | 26,059 | −36% | −16% |
| graphics-4bpp-32-det | 24,665 | 28,571 | −49% | −28% |
| full-4bpp-16 | 20,828 | 24,734 | −29% | −11% |
| full-4bpp-16-det | 23,358 | 27,264 | −42% | −22% |
| full-4bpp-32 | 24,018 | 27,924 | −45% | −25% |
| full-4bpp-32-det | 26,596 | 30,502 | −59% | −36% |

With the interrupts running for real, one core is worse still: Full 4bpp with
16 × 16 sprites takes 26,428 cycles, not 24,734
([Sprites on core 0](#sprites-on-core-0)).

Without sprites a line is fine. Full mode, 4bpp, the sum of the stage maxima:

- **Two layers:** 9,529 cycles, 13,435 with bus and line start. Margin 30% at
  302.4 MHz, 40% at 352 MHz.
- **One layer:** 4,543 cycles, 8,449 with bus and line start. Margin 56% and
  62%.

Sprites are what don't fit.

Bit depths
----------

Cycles on one core. The last column is the whole line with magnified sprites
and detailed collision, bus and line start included.

| Depth | Geometry | Layer 0 | Layer 1 | Sprites 16 | Sprites 16 det | Sprites 32 | Sprites 32 det | Line, worst (32 det) |
|---|---|--:|--:|--:|--:|--:|--:|--:|
| 1bpp | graphics | 1,828 | 3,564 | 8,615 | 11,214 | 11,613 | 14,276 | 26,594 |
| 2bpp | graphics | 2,277 | 3,979 | 8,702 | 11,357 | 11,699 | 14,424 | 27,547 |
| 4bpp | graphics | 2,312 | 4,072 | 9,592 | 12,082 | 12,777 | 15,289 | 28,571 |
| 8bpp | graphics | 1,690 | 3,100 | 9,042 | 11,789 | 12,164 | 14,859 | 26,550 |
| 1bpp | full | 2,248 | 4,390 | 9,126 | 11,768 | 12,124 | 14,856 | 28,314 |
| 2bpp | full | 2,786 | 4,896 | 9,196 | 11,890 | 12,193 | 14,981 | 29,426 |
| 4bpp | full | 2,846 | 5,021 | 10,087 | 12,617 | 13,272 | 15,850 | 30,502 |
| 8bpp | full | 2,070 | 3,817 | 9,492 | 12,294 | 12,614 | 15,390 | 28,062 |

§18 has the order right: 8bpp is the cheapest layer, 4bpp the dearest. The
spread is smaller than it implies, about 30% from cheapest to dearest, because
most of a cell's cost is the name, attribute and pattern fetches and the
priority merge, not the unpacking. Sprite cost barely depends on depth.

Still Open 2 — the 4bpp table
-----------------------------

| 4bpp layers | Graphics L0 | Graphics L1 | Full L0 | Full L1 |
|---|--:|--:|--:|--:|
| Table (8 KB) | 2,312 | 4,072 | 2,846 | 5,021 |
| Arithmetic | 2,744 | 4,253 | 3,380 | 5,247 |

The comparison is against the best arithmetic path the spike found, not a naive
one: one 32-bit load for the pattern row, and the eight nibbles spread to bytes
with shifts and a carry-free non-zero test.

**Answer: yes, it earns its place, by less than §18 assumed.** It saves 16% on
an opaque layer and 4% on a merged one: 760 cycles on a Full two-layer line.
8 KB is 1.5% of the SRAM. Keep it, and build it first, as §18 says. It does not
make 4bpp "about 2 cycles a pixel": the table-driven opaque layer is 8.9 cycles
a pixel in Full mode.

Clock preset
------------

**352 MHz** (PLL 1056 MHz ÷ 3, VREG 1.30 V). It gives 16% more cycles per line
than 302.4 MHz, and every figure below needs it. It changes nothing SPEC.md
specifies (§2 already lists it), and it is the first of Still Open 1's remedies.

What it costs is what Phases 8, 10 and 13 check: stability and temperature at
1.30 V over their ten- and thirty-minute runs. Neither preset showed a fault
here, but each suite runs for minutes, not half an hour. The flash SPI divider
is 4, as pico9918 sets it, so a firmware that ever touches flash at 352 MHz stays
in range.

Sprites on core 0
-----------------

The remedy recommended from the one-core figures, now built and measured. It
changes the firmware's shape (section 3 of the plan), not SPEC.md.

**How it works.**

1. **Line start.** Core 1's latch drains the journal and snapshots the registers
   as before, then evaluates the sprites.
2. **The boundary.** Core 1 picks a column `xs` on a 32-pixel word and posts the
   line to core 0 through the inter-core FIFO.
3. **In parallel.** Core 0 draws the sprites left of `xs` into a sprite line:
   indices, plus the priority bit that would hide each pixel. Meanwhile core 1
   builds both layers, then draws the sprites right of `xs` straight into the
   line. Both read the render copy, which cannot change until the next latch.
4. **The merge.** Core 1 waits for core 0, merges its sprite line in four-pixel
   words, publishes the collision status core 0 returned, and expands.

Each side keeps its own claim bitmap. Because `xs` is on a word boundary,
priority among sprites and collision stay exact within each side, and no pixel
belongs to both. Every status write stays on core 1.

**Choosing the boundary per line.** The best `xs` depends on the sprites. Too
far right and core 1 waits for core 0; too far left and core 1 runs late itself.
Three ways were tried:

1. **A position-aware estimate on core 1:** each column's sprite cost from the
   listed sprites' positions. It cost ~2,500 cycles a line on core 1, where
   every cycle adds to the latency.
2. **The same estimate on core 0,** in its parallel time. It still cost core 0
   ~2,400 cycles, and it trailed the best fixed boundary by up to 2,400.
3. **An O(1) rule on core 1,** which was kept. It assumes the listed sprites are
   spread evenly and balances their estimated cost against core 1's layers, as
   the last line measured them, plus measured fixed costs. It then moves `xs` a
   column toward whichever core finished first on the previous line. It costs a
   few tens of cycles, and lands within 170–1,700 cycles of the best fixed `xs`.

**Results.** 352 MHz, bus interrupt every 2 µs on core 1, VGA interrupt on core
0, 4,800 lines a scene, budget 22,371 cycles:

| Scene | One core | Best fixed split | at xs | Chosen per line | Late lines | Margin |
|---|--:|--:|--:|--:|--:|--:|
| graphics-1bpp-16 | 21,694 | 15,829 | 192 | 16,130 | 0 | 28% |
| graphics-1bpp-16-det | 24,776 | 16,719 | 192 | 18,406 | 0 | 18% |
| graphics-1bpp-32 | 25,668 | 17,431 | 192 | 17,608 | 0 | 21% |
| graphics-1bpp-32-det | 28,689 | 19,538 | 192 | 20,407 | 0 | 9% |
| graphics-2bpp-16 | 22,633 | 15,920 | 224 | 16,174 | 0 | 28% |
| graphics-2bpp-16-det | 25,948 | 17,649 | 192 | 18,426 | 0 | 18% |
| graphics-2bpp-32 | 26,557 | 18,550 | 192 | 19,517 | 0 | 13% |
| graphics-2bpp-32-det | 29,935 | 19,533 | 192 | 19,776 | 0 | 12% |
| graphics-4bpp-16 | 24,056 | 16,667 | 224 | 17,039 | 0 | 24% |
| graphics-4bpp-16-det | 27,154 | 18,269 | 192 | 19,171 | 0 | 14% |
| graphics-4bpp-32 | 28,017 | 19,328 | 192 | 20,179 | 0 | 10% |
| graphics-4bpp-32-det | 31,026 | 20,308 | 192 | 20,482 | 0 | 8% |
| graphics-8bpp-16 | 21,096 | 15,043 | 192 | 16,048 | 0 | 28% |
| graphics-8bpp-16-det | 24,535 | 16,967 | 192 | 17,712 | 0 | 21% |
| graphics-8bpp-32 | 25,031 | 17,487 | 192 | 17,729 | 0 | 21% |
| graphics-8bpp-32-det | 28,448 | 19,699 | 160 | 19,922 | 0 | 11% |
| full-1bpp-16 | 23,721 | 16,860 | 288 | 17,068 | 0 | 24% |
| full-1bpp-16-det | 26,925 | 18,190 | 256 | 19,200 | 0 | 14% |
| full-1bpp-32 | 27,626 | 18,706 | 256 | 20,266 | 0 | 9% |
| full-1bpp-32-det | 30,873 | 20,871 | 256 | 21,231 | 0 | 5% |
| full-2bpp-16 | 25,007 | 17,011 | 320 | 17,973 | 0 | 20% |
| full-2bpp-16-det | 28,258 | 19,366 | 288 | 19,681 | 0 | 12% |
| full-2bpp-32 | 28,882 | 20,060 | 256 | 20,385 | 0 | 9% |
| full-2bpp-32-det | 32,274 | 20,889 | 256 | 21,393 | 0 | 4% |
| full-4bpp-16 | 26,428 | 17,771 | 320 | 19,448 | 0 | 13% |
| full-4bpp-16-det | 29,559 | 20,033 | 288 | 20,255 | 0 | 9% |
| full-4bpp-32 | 30,214 | 20,705 | 288 | 20,962 | 0 | 6% |
| full-4bpp-32-det | 33,469 | 21,574 | 256 | 21,850 | 0 | 2% |
| full-8bpp-16 | 22,873 | 16,440 | 256 | 16,949 | 0 | 24% |
| full-8bpp-16-det | 26,353 | 18,303 | 224 | 18,469 | 0 | 17% |
| full-8bpp-32 | 26,805 | 18,810 | 256 | 19,087 | 0 | 15% |
| full-8bpp-32-det | 30,290 | 20,129 | 224 | 21,304 | 0 | 5% |

The split with a lower limit, Full mode, 4bpp, boundary chosen per line:

| Full mode, 4bpp, split, chosen per line | SPRLIMIT 32 | Margin | SPRLIMIT 16 | Margin |
|---|--:|--:|--:|--:|
| full-4bpp-16 | 19,448 | 13% | 16,475 | 26% |
| full-4bpp-16-det | 20,255 | 9% | 16,501 | 26% |
| full-4bpp-32 | 20,962 | 6% | 16,800 | 25% |
| full-4bpp-32-det | 21,850 | 2% | 16,862 | 25% |

How the boundary moves the line, Full mode, 4bpp:

| Full mode, 4bpp | xs | Latency | Late lines | Core 1 waited | Core 0 work |
|---|--:|--:|--:|--:|--:|
| full-4bpp-16 | one core | 26,428 | 320 | 0 | — |
| full-4bpp-16 | 320 | 17,771 | 0 | 2,292 | 10,854 |
| full-4bpp-16 | 288 | 18,337 | 0 | 436 | 10,828 |
| full-4bpp-16 | 256 | 19,386 | 0 | 115 | 9,877 |
| full-4bpp-16 | 224 | 20,250 | 0 | 115 | 8,656 |
| full-4bpp-16 | 192 | 21,264 | 0 | 116 | 7,692 |
| full-4bpp-16 | 160 | 22,058 | 0 | 115 | 6,509 |
| full-4bpp-16 | per line | 19,448 | 0 | 1,360 | 10,842 |
| full-4bpp-16-det | one core | 29,559 | 320 | 0 | — |
| full-4bpp-16-det | 320 | 20,694 | 0 | 5,104 | 13,727 |
| full-4bpp-16-det | 288 | 20,033 | 0 | 2,642 | 13,217 |
| full-4bpp-16-det | 256 | 20,035 | 0 | 116 | 12,008 |
| full-4bpp-16-det | 224 | 21,204 | 0 | 116 | 10,476 |
| full-4bpp-16-det | 192 | 22,641 | 118 | 116 | 9,659 |
| full-4bpp-16-det | 160 | 23,661 | 313 | 116 | 8,193 |
| full-4bpp-16-det | per line | 20,255 | 0 | 2,473 | 13,207 |
| full-4bpp-32 | one core | 30,214 | 640 | 0 | — |
| full-4bpp-32 | 320 | 21,115 | 0 | 5,497 | 14,043 |
| full-4bpp-32 | 288 | 20,705 | 0 | 2,600 | 13,843 |
| full-4bpp-32 | 256 | 20,788 | 0 | 115 | 12,592 |
| full-4bpp-32 | 224 | 21,933 | 0 | 115 | 10,990 |
| full-4bpp-32 | 192 | 23,472 | 478 | 115 | 9,718 |
| full-4bpp-32 | 160 | 24,680 | 640 | 115 | 8,188 |
| full-4bpp-32 | per line | 20,962 | 0 | 2,421 | 13,869 |
| full-4bpp-32-det | one core | 33,469 | 640 | 0 | — |
| full-4bpp-32-det | 320 | 24,044 | 640 | 8,292 | 16,880 |
| full-4bpp-32-det | 288 | 23,230 | 640 | 4,558 | 16,257 |
| full-4bpp-32-det | 256 | 21,574 | 0 | 999 | 14,741 |
| full-4bpp-32-det | 224 | 23,110 | 320 | 116 | 12,864 |
| full-4bpp-32-det | 192 | 24,949 | 640 | 116 | 11,263 |
| full-4bpp-32-det | 160 | 26,429 | 640 | 116 | 9,794 |
| full-4bpp-32-det | per line | 21,850 | 0 | 970 | 14,753 |

**Do the cores slow each other down?** Barely. With all the sprites on one core:

| 4bpp, all sprites on one core | Core 1 alone, interrupts off | Core 0, core 1 idle | Core 0, core 1 building layers |
|---|--:|--:|--:|
| graphics-4bpp-16 | 10,084 | 10,188 | 10,321 |
| graphics-4bpp-16-det | 12,865 | 12,994 | 13,123 |
| graphics-4bpp-32 | 13,162 | 13,379 | 13,465 |
| graphics-4bpp-32-det | 15,955 | 16,139 | 16,243 |
| full-4bpp-16 | 10,608 | 10,706 | 10,854 |
| full-4bpp-16-det | 13,436 | 13,533 | 13,727 |
| full-4bpp-32 | 13,687 | 13,890 | 14,043 |
| full-4bpp-32-det | 16,554 | 16,730 | 16,880 |

The VGA interrupt costs core 0 about 1%. Core 1 working at the same time costs
it another 0.5–1.5%: the shared bus fabric to SRAM is not a bottleneck. Waiting
for the other core by polling SRAM or SIO made no measurable difference, so the
spike waits on the FIFO's status in SIO.

**What it doesn't yet show.**

- USB traffic on core 0 during a line: in debug builds, snapshot streaming.
  Phase 8 loads the link while measuring.
- Thirty minutes of it, and the PRO's own thermal behaviour: Phases 8 and 13.
- A real PIO bus handler in place of the stand-in: Phase 11.
- The core 0 start-up and the wait on core 1 at priority, with the latch as an
  interrupt: Phase 8's firmware. The spike's line loop runs back to back in
  thread mode, which is at least as tight as a latch every 63.56 µs.

Still Open 1 — the remedies weighed
-----------------------------------

The trigger: on one core, the Full mode worst case leaves −29% of the line at
302.4 MHz against the 25% the plan requires. In Still Open 1's order, on one
core (stage timing plus the bus and line-start stand-ins):

| Full mode, 4bpp | Sprites | Layers | SPRLIMIT | Line, worst | Margin @ 302.4 | Margin @ 352 |
|---|---|---|--:|--:|--:|--:|
| full-4bpp-16 | 16 × 16 | two | 32 | 24,734 | −29% | −11% |
| full-4bpp-16-lim24 | 16 × 16 | two | 24 | 22,586 | −18% | −1% |
| full-4bpp-16-lim16 | 16 × 16 | two | 16 | 19,999 | −4% | 11% |
| full-4bpp-16-lim8 | 16 × 16 | two | 8 | 17,460 | 9% | 22% |
| full-4bpp-16-l0only | 16 × 16 | one | 32 | 19,778 | −3% | 12% |
| full-4bpp-16-lim16-l0only | 16 × 16 | one | 16 | 15,029 | 22% | 33% |
| full-4bpp-16-det | 16 × 16, detailed | two | 32 | 27,264 | −42% | −22% |
| full-4bpp-16-det-lim24 | 16 × 16, detailed | two | 24 | 24,574 | −28% | −10% |
| full-4bpp-16-det-lim16 | 16 × 16, detailed | two | 16 | 21,430 | −12% | 4% |
| full-4bpp-16-det-lim8 | 16 × 16, detailed | two | 8 | 18,260 | 5% | 18% |
| full-4bpp-16-det-l0only | 16 × 16, detailed | one | 32 | 22,308 | −16% | 0% |
| full-4bpp-16-det-lim16-l0only | 16 × 16, detailed | one | 16 | 16,460 | 14% | 26% |
| full-4bpp-32 | magnified | two | 32 | 27,924 | −45% | −25% |
| full-4bpp-32-lim24 | magnified | two | 24 | 24,402 | −27% | −9% |
| full-4bpp-32-lim16 | magnified | two | 16 | 21,839 | −14% | 2% |
| full-4bpp-32-lim8 | magnified | two | 8 | 18,711 | 3% | 16% |
| full-4bpp-32-l0only | magnified | one | 32 | 22,961 | −19% | −3% |
| full-4bpp-32-lim16-l0only | magnified | one | 16 | 16,883 | 12% | 25% |
| full-4bpp-32-det | magnified, detailed | two | 32 | 30,502 | −59% | −36% |
| full-4bpp-32-det-lim24 | magnified, detailed | two | 24 | 26,365 | −37% | −18% |
| full-4bpp-32-det-lim16 | magnified, detailed | two | 16 | 23,355 | −22% | −4% |
| full-4bpp-32-det-lim8 | magnified, detailed | two | 8 | 19,593 | −2% | 12% |
| full-4bpp-32-det-l0only | magnified, detailed | one | 32 | 25,539 | −33% | −14% |
| full-4bpp-32-det-lim16-l0only | magnified, detailed | one | 16 | 18,399 | 4% | 18% |

1. **352 MHz.** Adopted. Alone it is not enough: −11% with plain 16 × 16
   sprites, −36% at worst.
2. **A lower default `SPRLIMIT`.** On one core at 352 MHz, 16 still leaves −4%
   at worst; 8 reaches 12–22%. Nothing on the list reaches 25% for every sprite
   setting. A default is only a default: software may set 32.
3. **Full mode as a single-layer mode.** At 352 MHz: 12% for plain sprites, −14%
   at worst, which is worth less than halving `SPRLIMIT`. It is also the one
   remedy that changes behaviour (risk 1), and **it breaks the emulator's
   `vdp-layers` fixture**, which scrolls two 4bpp layers in Full mode.
4. **Sprites on core 0** ([above](#sprites-on-core-0)), not on the list. Every
   worst case fits at 352 MHz with `SPRLIMIT` 32, with real interrupt load and no
   late lines. The margin is 13% down to 2% in Full mode 4bpp; 4% or more at
   Full mode's other depths; 8% or more in Graphics mode. With `SPRLIMIT` 16 it is
   25–26% for every Full mode 4bpp setting. SPEC.md doesn't change.

**Recommendation.**

- **Adopt 352 MHz and the core 0 sprite split,** with the boundary chosen per
  line. It is the only remedy that keeps all of SPEC.md's features and fits
  every worst case the spike could build.
- **Decide the threshold.** The plan's 25% margin is met only with `SPRLIMIT`
  16. Two ways to go:
  - **(a) Keep `SPRLIMIT` 32** and accept a 2% margin for the harshest case.
    That case is 32 magnified, fully solid, mutually colliding 4bpp sprites with
    detailed collision, on a Full mode two-layer line, while the CPU writes the
    VDP every 2 µs without pause. No program is likely to make it, but late-line
    behaviour would still need specifying.
  - **(b) Reset `SPRLIMIT` to 16** (§5), keeping 32 reachable by software,
    which then accepts the thinner margin. This meets the plan's 25% and changes
    one reset value. It weakens §9's "up to 32 sprites on a line" for legacy
    programs, which see 16 by default: still four times a TMS9918's four.

  I lean to (b): it keeps the plan's own threshold honest, and costs a
  one-register write for software that wants more.
- **Either way,** §18 should say what a late line does (the plan already counts
  them), and section 3 of PLAN.md gains the core split. SPEC.md §18, Still Open
  1 and 2, and §5 if (b), are then rewritten once, in all three copies (ground
  rules 2 and 7).

Why §18 was wrong
-----------------

- **Cycles per instruction.** §18 counted instructions and priced them at a
  cycle each. On this M33 in SRAM, loads and stores cost about two. The first
  pass's expansion loop was five instructions a pixel and measured 7.5 cycles.
  Sprite evaluation, which touches memory once a slot, measures one cycle per
  instruction. Table-driven code pays for every table load.
- **Instruction counts.** A cell is not a few loads and stores. Each one needs
  its name, attribute and pattern fetched with 64 KB wrap, the flips applied,
  and the priority class written beside the indices so later sources can
  compete (§12). That runs to 60–70 instructions a cell in the merge path.
- **Detailed collision** was priced as an owner index in the line buffer, ~500
  cycles. Exact pairs, settled once a line from the pixels each sprite hit,
  cost ~2,500.
- **Magnification** doubles each sprite's pixel work, as §18 says, and adds
  doubling the decoded row: 3,200 against §18's 2,600.
- **Bus service** is ~95 cycles an access against §18's ~38, and exception
  entry and exit are most of it. The real handler's MMIO may cost more or less.
  Phase 11 measures it.
- **The one-core arithmetic understates the interrupts.** With the bus
  interrupt running for real, one core takes 26,428 cycles for the line the
  arithmetic puts at 24,734.

Done when
---------

| Check | Result |
|---|---|
| Every row of §18's table has a measured figure at both clocks | ✅ [§18, measured](#18-measured). Cycle counts are identical at both clocks; margins are given for each |
| Still Open 2 has an answer | ✅ Yes: 16% on an opaque layer, 4% merged; keep the table |
| A clock preset is chosen | ✅ 352 MHz |
| If the Full mode worst case leaves < 25% at 302.4 MHz, Still Open 1's remedies are weighed, in order, before Phase 3 | Triggered (−29% on one core). Weighed [above](#still-open-1--the-remedies-weighed), with the core 0 split measured. The owner took the recommendation: draft 0.4 |
| Host suite green | ✅ `host` and `host-asan`: `test_placeholder`, `spike_reference` |

Differences from the plan
-------------------------

- **The renderer took several passes.** The first pass's figures are kept
  (`phase-01-pass1.txt`) but don't stand for the budget.
- **A host check was added:** `spike_reference` under CTest, which the plan
  didn't ask for. A spike whose output is never compared could be fast because
  it is wrong.
- **Bus service and line start are stand-ins.** The phase has no bus; the
  shapes follow section 3.
- **The worst case is harsher than §18's composite row:** 64 covering slots,
  every sprite pixel solid, every neighbour overlapping. §18 describes the same
  line but prices less of it.
- **Extra scenes** measure the remedies: `SPRLIMIT` 8, 16, 24 and layer 1 off.
- **The core 0 split was built and measured**, with real interrupt load on both
  cores. The plan's list had no such remedy.
- **`-O3` was also measured**, for comparison.
- **SPEC.md draft 0.4 and PLAN.md section 3** carry the choice above (ground
  rules 2 and 7).

Reproducing
-----------

```sh
cmake --workflow --preset host                   # spike_reference: three build paths against the reference
cmake --preset pico2 && cmake --build --preset pico2
picotool load -x -f build/pico2/spike/picovdp_spike.uf2
node spike/capture.mjs docs/results/phase-01-final.txt r    # stages, both clocks, ~2 min
node spike/capture.mjs docs/results/phase-01-split.txt s    # the split at 352 MHz, ~7 min
node spike/report.mjs docs/results/phase-01-final.txt docs/results/phase-01-pass1.txt docs/results/phase-01-split.txt
```

`-DPICOVDP_SPIKE_OPT=-O3` on the configure builds the `-O3` variant. The Pico 2
is left running the `-O2` spike image; Phase 8 replaces it.

| File | Holds |
|---|---|
| `phase-01-final.txt` | stage timing, `-O2`, 60 scenes at both clocks |
| `phase-01-split.txt` | the split at 352 MHz: one core, fixed boundaries, per-line choice and diagnostics, 36 scenes |
| `phase-01-O3.txt` | stage timing, `-O3`, 60 scenes at both clocks |
| `phase-01-pass1.txt` | the first, per-pixel renderer, `-O2`, 40 scenes |
