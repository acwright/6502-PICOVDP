Phase 10 — The firmware on the PRO, no bus
==========================================

**Status:** done on 2026-09-23. Every criterion in PLAN.md's Phase 10 is met.
One thing behaved worse than it did on the Pico 2, and it is the debug link
rather than the card: [The link under a long
stream](#the-link-under-a-long-stream).

Phase 8 again, on the card it was written for: the same firmware, the same
oracle, the same worst cases, now on a PICO9918 PRO v2.0 with its DAC and HDMI
dongle attached — and with the picture on the monitor scored against the
goldens for the first time.

**The headline.**

- **The goldens reproduce on the PRO.** All eighteen checkpoints, replayed by
  injection from a cold reset: the index frame as it went to VGA, all 64 KB of
  VRAM, the 128 registers and `STAT0` exact, and every read and every `/INT`
  level the traces carry — 4,586,873 operations, 4,494,507 of them reads, in
  92 seconds.
- **And they reproduce on the monitor.** Each checkpoint's picture was grabbed
  off the capture card and scored against its golden through §11's palette.
  Where the picture has settled areas, 99.85% or better of those pixels are
  within 8 levels of the golden — half a step of the DAC — with the worst pixel
  17. The golden's own `0x0BGR` mapping explained every one of the twenty
  pictures better than any wrong mapping tried.
- **The DAC is wired as §2 says.** The DAC card measures each channel's sixteen
  levels with the other two dark: every ramp rises at every step, and the other
  two channels never rise above 0.2 of 255. Its one-pixel stripes show each
  card line occupying exactly two VGA lines, both identical, and each card
  pixel exactly two pixels across.
- **All 256 palette entries reach the monitor, and every ramp is a ramp.** The
  palette card measures each of §11's entries on its own swatch: all 256 come
  back within 12.5 levels of their colour on average, and over all 240 steps of
  the sixteen families there is not one that should brighten and does not. 251
  of the 256 come back as distinct colours, and four of the five coincidences
  are entries §11 defines identically. The card is captured for the owner to
  judge the hue ramps (PLAN.md's Still Open 3).
- **The RP2354A is the RP2350A.** Over the 36 worst-case scenes the PRO's worst
  line differs from the Pico 2's by a mean of 79 cycles in about 17,000: 0.4% of
  a line. Nothing about the PRO's package, its 2 MB of in-package flash or its
  board changes the budget. What *does* move a number, by 30%, is which board
  header the image was built with — [below](#where-the-layout-does-show-the-latch-under-a-font-load).
- **Ten minutes of snapshots on one scene is more than the debug link can
  carry.** The 36 scenes streamed for 605 seconds without trouble, and the
  harshest scene ran its own ten minutes — 8,631,137 rows, none late — but with
  snapshots streaming as well it gave up at 9 minutes 24 seconds, on a
  `SNAPSHOT` request the board never answered. The card is not what failed, and
  the link has to be fixed before Phase 13 streams for thirty minutes.
- **A stray sprite, found by a bench card.** `SPRCTRL` resets to `$27` —
  sprites on — and `SPRATTR` to `$00`, so a program that sets up a picture at $0000 and
  never touches the sprite registers draws a sprite out of its own name table.
  The palette card did, and the capture showed it. Correct per §10, and now
  something the cards turn off deliberately.

Contents: [What was built](#what-was-built) ·
[The goldens on the PRO](#the-goldens-on-the-pro) ·
[The picture on the monitor](#the-picture-on-the-monitor) ·
[The bench cards](#the-bench-cards) ·
[The worst-case scenes](#the-worst-case-scenes) ·
[The link under a long stream](#the-link-under-a-long-stream) ·
[A late line on purpose](#a-late-line-on-purpose) ·
[Faults and reflashing](#faults-and-reflashing) ·
[Done when](#done-when) ·
[Differences from the plan](#differences-from-the-plan) ·
[For later phases](#for-later-phases) · [Reproducing](#reproducing)

---

What was built
--------------

Nothing in `core/` changed in this phase, and `firmware/` changed in one
debug-only place. The `pro-debug` image is the `pico2` image with a different
board header; it built, flashed and ran first time, on the pins `picotool info`
read out of the stock firmware in Phase 9. What was built is on the host, to see
the picture:

| Part | What it is |
|---|---|
| `tools/lib/screen.mjs` | A capture against a golden: where the picture sits, the path's black level and gain, the pixels far enough from a colour change to have settled, and five wrong DAC mappings to beat. Holds the tolerance |
| `tools/lib/cards.mjs` | The bench cards as programs of port operations: the palette card and the DAC card |
| `tools/card.mjs` | Draws a card with the reference, replays it through the core, checks it is static, and writes a trace and a golden of the oracle's shape into `bench/cards/`. `--check` holds the cards that are there to the core, with no emulator needed (CTest `cards_pinned`) |
| `tools/vdpctl.mjs` | `inject --capture [--pictures DIR]`, `card <name>`; `PICOVDP_PRESET` picks the image `flash` sends and the ELF a fault is symbolised against |
| `tools/replay.mjs` | A tool when run, a library when imported, so `card.mjs` replays through the same code the fixtures do |
| `firmware/renderer.c` | `scene_line` holds the bus stand-in off while the scene's own program writes ([below](#the-stand-ins-own-race-found-here)). Debug builds only |
| `bench/cards/` | The two cards: a trace, an index frame, 64 KB of VRAM, the structural JSON and a PNG each |
| `docs/BENCH.md` | Section 6 gains the capture comparison, the bench cards and the path's measured numbers; section 7 gains Phase 10's order |

---

The goldens on the PRO
----------------------

`vdpctl inject all --capture`. Each checkpoint is replayed from its trace's cold
reset; the board captures the golden frame as it goes to VGA and records its
registers and `STAT0` at the checkpoint.

| Checkpoint | Frame | Operations | Reads | Result |
|---|--:|--:|--:|---|
| `bios/ok` | 59 | 5,027 | 1,837 | **exact** |
| `bios/screenful` | 278 | 5,555 | 1,837 | **exact** |
| `bios/scroll` | 479 | 6,055 | 1,837 | **exact** |
| `wizardslab/frame-60` | 59 | 93,133 | 89,044 | **exact** |
| `wizardslab/frame-180` | 179 | 295,200 | 291,075 | **exact** |
| `wizardslab/frame-300` | 299 | 497,081 | 492,902 | **exact** |
| `wizardslab/frame-600` | 599 | 1,002,238 | 997,951 | **exact** |
| `vdp-modes/text` | 35 | 62,171 | 60,205 | **exact** |
| `vdp-modes/compact` | 98 | 171,311 | 167,727 | **exact** |
| `vdp-modes/graphics` | 162 | 281,909 | 276,289 | **exact** |
| `vdp-modes/full` | 226 | 393,122 | 385,262 | **exact** |
| `vdp-layers/parallax` | 89 | 123,648 | 117,840 | **exact** |
| `vdp-layers/scroll-bit8-l1` | 179 | 288,854 | 282,146 | **exact** |
| `vdp-layers/occluded` | 239 | 399,002 | 391,694 | **exact** |
| `vdp-layers/scroll-bit8-l0` | 299 | 509,139 | 501,231 | **exact** |
| `vdp-font/reset` | 32 | 55,617 | 53,665 | **exact** |
| `vdp-font/loaded` | 96 | 168,592 | 162,660 | **exact** |
| `vdp-font/relocated` | 160 | 229,219 | 219,305 | **exact** |

Exact means what it meant in Phase 8: the 76,800 indices of the golden frame
with no late row in it, the 64 KB of VRAM, the 128 registers through their
aliases, `STAT0`, every read returning the trace's value, and `/INT` after
every operation and at every line the trace moved it. Fifteen reads of `STAT5`
across the eighteen are the firmware's own version byte and are not compared
(PLAN.md section 4).

`docs/results/phase-10/inject-capture.txt` and `.json` hold the run.

---

The picture on the monitor
--------------------------

The injection proves the frame digitally. What it cannot see is the rest of the
way: the DAC's twelve pins, the ×2 across, the two VGA lines a card line is sent
as, and §11's palette expansion. That is what the capture card is for — and it
is an analog path, so the comparison has to be made in terms the path can carry.

### The method

`tools/lib/screen.mjs`, run for every checkpoint by `inject --capture`:

1. **Where the picture sits.** The golden, expanded through the checkpoint's own
   palette to 640 × 480, is searched against the capture over ±8 pixels.
2. **Black level and gain.** One scale and one offset per channel, by least
   squares. A wrong bit order is not a scale, which is the point of step 4.
3. **Settled pixels.** A colour change takes this path about eight capture
   pixels to settle. Pixels that far from any change in the golden are compared
   one for one; everything nearer is counted and left out.
4. **The whole picture,** edges and ringing included, against the golden and
   against five wrong mappings: red and blue swapped, green and blue swapped,
   the channels rotated, every nibble reversed, and red's nibble reversed. The
   golden's own mapping has to beat all five.

### What the path does

Measured here, and now the tolerance in `screen.mjs`:

| | |
|---|---|
| Where the picture sits | −1 to 4 pixels across, 0 to 3 lines up. Two things move it, neither the firmware's: the dongle resamples at a fraction of a pixel, so the integer answer depends on what the picture is made of, and where it locks sync changes when the board is reset — the whole set moved a pixel across and a line down after a reflash, and moved together |
| Two grabs of one picture | 0.24 levels apart, worst pixel 76: the capture is repeatable |
| How far a colour change reaches | 8 capture pixels across — 4 card pixels, about 300 ns — and about 6 down |
| How far a change of brightness alone reaches | nothing at all, down: one-pixel black and white stripes come back two exact rows at a time (below). The path carries chroma at half resolution both ways, so it is colour that spreads, not light |
| A settled pixel | 99.85% or better within 8 levels on every checkpoint, worst pixel 17 |
| A whole picture | 2.1 to 30.9 levels of mean error |
| The golden against the best wrong mapping | better on every picture, by 1.005× to 2.2× on the oracle's and 2.3× and 5.3× on the two bench cards. A picture that cannot tell two mappings apart gives a margin near 1: `wizardslab`'s greens and greys barely notice green and blue changing places, which is why the DAC card exists |

### Every checkpoint

Offset, mean error over the whole picture, and the settled pixels:

| Checkpoint | Offset | Picture MAE | Settled px | Within 8 | Worst | Best wrong mapping |
|---|--:|--:|--:|--:|--:|--:|
| `bios/ok` | 3,−1 | 2.4 | 243,679 | 100.00% | 3 | 4.5 |
| `bios/screenful` | 3,−1 | 2.9 | 217,748 | 100.00% | 2 | 5.2 |
| `bios/scroll` | 3,−1 | 2.1 | 222,591 | 100.00% | 4 | 2.4 |
| `wizardslab/frame-60` | 3,−1 | 15.7 | 119,647 | 100.00% | 8 | 15.7 |
| `wizardslab/frame-180` | 3,−1 | 15.7 | 119,647 | 100.00% | 8 | 15.7 |
| `wizardslab/frame-300` | 3,−1 | 15.6 | 119,647 | 100.00% | 8 | 15.7 |
| `wizardslab/frame-600` | 3,−1 | 15.6 | 119,647 | 100.00% | 8 | 15.7 |
| `vdp-modes/text` | 3,−1 | 21.9 | 89,061 | 99.98% | 12 | 29.1 |
| `vdp-modes/compact` | 3,−1 | 17.9 | 74,308 | 99.99% | 10 | 39.0 |
| `vdp-modes/graphics` | 3,−1 | 22.2 | 42,504 | 100.00% | 3 | 48.8 |
| `vdp-modes/full` | 4,−1 | 30.9 | **0** | — | — | 61.8 |
| `vdp-layers/parallax` | 1,−1 | 13.6 | 101,347 | 99.85% | 17 | 23.6 |
| `vdp-layers/scroll-bit8-l1` | 1,−1 | 13.4 | 93,552 | 99.95% | 17 | 23.7 |
| `vdp-layers/occluded` | 1,−3 | 14.5 | 92,686 | 99.94% | 12 | 24.6 |
| `vdp-layers/scroll-bit8-l0` | 1,−1 | 13.7 | 99,967 | 99.98% | 12 | 24.0 |
| `vdp-font/reset` | 3,−1 | 7.2 | 167,532 | 100.00% | 10 | 10.7 |
| `vdp-font/loaded` | 3,−1 | 7.2 | 167,532 | 100.00% | 10 | 10.7 |
| `vdp-font/relocated` | 3,−1 | 7.2 | 167,532 | 100.00% | 10 | 10.8 |

Four things in that table are worth saying out loud.

**`vdp-modes/full` has no settled pixels at all.** Its picture changes colour
every two card pixels over the whole frame, so nothing in it is ever eight
capture pixels from a change, and there is no pixel this path can carry
faithfully. Its 30.9 is ringing at edges, everywhere; the difference image is a
grid of cell boundaries with black between them. That is the bench's limit, not
the firmware's — the frame itself was compared index for index and was exact.

**The three `vdp-font` checkpoints score identically,** and so do the four
`wizardslab` ones. Their goldens differ in 189, 378 and 144 of 76,800 pixels —
under half a percent — so their statistics coincide. The captures are not the
same picture: `vdp-font`'s three differ from one another in 2,910 and 4,655
capture pixels by more than 16 levels.

**The offsets in this table are one sync lock's.** Reflashing the board and
running the same checkpoints again put every picture at −1,0 instead, with the
same errors: `bios/ok` 1.9 against 2.4, 100.00% of its settled pixels within 8.
Where the dongle decides the picture starts is not something the card is being
judged on, which is why the tolerance allows five pixels either way.

**Most of the oracle's settled area is one colour.** Where a picture's settled
pixels are all the same colour there is no gain to fit, and the comparison is
made with the gain at 1 and only the black level removed — a stricter test, not
a looser one. The pictures that *do* carry colour into their settled areas are
`vdp-layers`' four (gain 1.04–1.13 per channel) and the two bench cards, which
is what the cards are for.

Captures kept: `docs/results/phase-10/shots/`.

---

The bench cards
---------------

The oracle's fixtures are the emulator's programs, and they are what proves the
card draws what SPEC.md says. A bench card proves something else — what reaches
the monitor — so it is made of large flat areas of known colour. Each is drawn
by the reference (`Video.ts`), replayed through this repository's core, checked
to be static, and written into `bench/cards/` as a trace and a golden of the
oracle's shape. `vdpctl card <name>` then injects and captures it like any
checkpoint.

### The DAC card

Its palette is its own: entries `$00`–`$0F` are red 0–15, `$10`–`$1F` green,
`$20`–`$2F` blue, `$30`–`$3F` grey, and everything else black. So each channel's sixteen levels are
measured **with the other two dark**.

| Channel | $0 | $1 | $2 | $3 | $4 | $5 | $6 | $7 | $8 | $9 | $A | $B | $C | $D | $E | $F |
|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| red | 0 | 12 | 27 | 46 | 69 | 89 | 109 | 130 | 139 | 159 | 183 | 204 | 226 | 232 | 238 | 243 |
| green | 0 | 9 | 28 | 48 | 65 | 84 | 108 | 125 | 132 | 153 | 173 | 193 | 211 | 230 | 251 | 255 |
| blue | 0 | 15 | 34 | 51 | 72 | 89 | 111 | 136 | 141 | 161 | 185 | 206 | 225 | 236 | 239 | 242 |
| grey | 0 | 5 | 25 | 42 | 60 | 79 | 97 | 116 | 126 | 140 | 164 | 179 | 196 | 215 | 235 | 252 |

Each figure is the mean over that level's settled pixels, of which the card has
106,664 spread over its 64 entries.

**Every ramp rises at every step, and the other two channels never rise above
0.2 of 255.** There is no cross-talk between the twelve pins at all, in either
direction: a red level puts nothing on green or blue. That is the `0x0BGR` bit
order on GPIO 2–13, proved by arithmetic rather than by eye — and it agrees with
the pin map the stock firmware reported in Phase 9.

The path is not linear, and the DAC card is where that shows: red `$C` reads 226
where `$F` reads 243, so the top four levels of red and of blue are compressed
into the range the rest of the ramp covers in one step. Grey, which is all three
channels together, stays straight to 252. It is the saturated-primary corner of
the capture chain, and it is why the card's own settled tolerance is 32 levels
where an oracle picture's is 8: the card measures the curve rather than being
measured by it. Nothing in it is the firmware's doing — the frame was exact.

### The stripes: line doubling and the ×2 across

The card's last band is one-pixel stripes. Mean luminance per capture row,
across the horizontal band:

```
  … 251.0 251.0 0.2 0.2 251.0 251.0 0.2 0.2 251.0 251.0 0.2 0.2 …
```

Two rows on, two rows off, exactly: **each card line is sent as two VGA lines,
and the two are identical.** Down the vertical band, per capture column:

```
  252.4 148.1 1.2 77.4 | 252.7 157.5 2.8 63.6 | 251.9 172.1 1.4 48.3 | …
```

Period four, peaks at full white, troughs at 1: each card pixel is two pixels
across. The two middle columns are the path's bandwidth — a one-pixel stripe is
the hardest thing it carries — not the card's.

### The palette card

All 256 entries of §11, 16 to a row, as swatches of 16 × 8 pixels, with the
family down the side and the step across the top. A swatch that short has
nothing left at the reach the gate uses, so the per-entry colours are measured
two rows in instead — which the card itself shows makes no difference, the
means moving by 0.2 of a level between two rows and five. **Every one of the 256
entries is measured, separately, on the monitor.**

| | |
|---|---|
| Entries with settled pixels of their own | **256** of 256, about 110 pixels each |
| Settled pixels, at the gate's reach | 69,789, 100% of them within 4 levels |
| Gain, per channel | 0.994 / 0.982 / 0.985 |
| Black level, per channel | −2.2 / −3.9 / −2.2 |
| An entry's colour against §11's, after that | 12.5 levels on average, **41.3 at worst** (entry `$49`, `$FF3`, whose blue reads 7 where 51 was sent) |
| Entries within half a DAC step | 93 of 256; within a whole step, 191 |
| Best wrong mapping | 47.3, against the golden's 9.0 |

Twelve levels on average is two thirds of a DAC step, and the worst is 2.4 steps
— on the same path whose own response the DAC card measured as an S-curve with
226, 232, 238, 243 for red's top four levels. The palette card inherits that
curve; it does not add to it.

What the card says about §11 itself, which is what it is for:

- **Every ramp rises.** Over all sixteen families and all 240 steps between
  consecutive entries, there is not one that should be brighter than the step
  before it and comes back no brighter. The ramps are ramps on a monitor.
- **Almost every colour §11 distinguishes comes back distinguishable.** 251
  distinct colours were captured from 256 entries. Four of the five coincidences
  are entries §11 defines identically — `$00`, `$01` and `$10` are all `$000`,
  `$08` and `$2A` are both `$F55`, `$0E` and `$1C` are both `$CCC`. The fifth is
  real: `$67` `$0F0` and `$68` `$2F2` both come back as full green, because the
  path has no room left above `$0F0` to put the difference. Seven pairs of
  different entries land within 4 levels of each other, and which pairs those
  are moves a little from capture to capture.

`docs/results/phase-10/shots/palette-palette-capture.png` is the picture
PLAN.md's **Still Open 3** — are the hue ramps usable — is to be judged on. The
arithmetic above says the ramps survive the path; whether they are *usable* is a
judgement about the palette, and it is the owner's to make on a real monitor.

### The stray sprite

The first palette card drew an 8 × 8 block of spring green at card (24, 24)
that nothing in the card's program put there. It was in the golden as well as
in the capture — the board and `Video.ts` agreed exactly — so it was not a
capture artefact and not a firmware fault:

`SPRCTRL` resets to `$27`, sprites enabled, and `SPRATTR` to `$00`. A card that
puts its name table at $0000 and never writes a sprite register is telling the
sprite engine that its attribute table is that name table. Sprite 0's four bytes
were four paper cells, $17 $17 $17 $17 — Y 23, X 23, pattern $17 — and the card
drew it, correctly, where §10 says. The cards now write `SPRCTRL` = `$26` and
say why.

---

The worst-case scenes
---------------------

Phase 1's 36 worst-case scenes, built through the ports by `firmware/scenes.c`
and drawn by the real core on both cores, with `FONT` written for both layers
every frame (§7) and `SNAPSHOT` streaming over USB throughout. Every snapshot is
checked against `vdp-scene`'s drawing of the frame its state names.

**Ten minutes, all 36 scenes** — `vdpctl scenes --seconds 16.7 --fonts --stream`.
A display line is 22,372 cycles:

| Scene | Rows built | Late | Latency max | Spare | Core 1 half | Core 0 half | Snapshots | Rows wrong |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| `graphics-1bpp-16` | 240,989 | 0 | 14,295 | 36% | 9,506 | 10,336 | 112 | 0 |
| `graphics-4bpp-32-det` | 241,899 | 0 | 19,588 | 12% | 14,140 | 15,587 | 112 | 0 |
| `full-2bpp-32-det` | 241,883 | 0 | 20,594 | 8% | 15,080 | 16,624 | 112 | 0 |
| `full-4bpp-32-det` | 241,907 | 0 | 20,404 | 9% | 15,086 | 16,636 | 112 | 0 |
| `full-8bpp-32-det` | 241,921 | 0 | 19,167 | 14% | 14,012 | 15,544 | 112 | 0 |
| `full-4bpp-16-lim16` | 241,904 | 0 | 13,372 | 40% | 9,056 | 10,475 | 112 | 0 |
| `full-4bpp-32-det-lim16` | 241,892 | 0 | 15,383 | 31% | 11,205 | 12,421 | 112 | 0 |

Seven of the 36; the rest are between them, at 8% to 40% spare. **Total:
8,702,674 rows, 0 late, 0 latches merged, 4,034 snapshots, 0 rows wrong**, in
605 seconds.

**Ten minutes, the harshest scene alone** — `vdpctl scenes --seconds 600 --fonts
--only full-4bpp-32-det`: **8,631,137 rows, 0 late, 0 latches merged**, latency
max 20,052, 10% spare.

With `--stream` on top of that it does not finish, and the card is not why: the
best of three attempts ran 9 minutes 24 seconds and 8,499,066 rows with no late
row, no merged latch, no missed bell, no journal overflow and no raster slip
before a `SNAPSHOT` request went unanswered and `vdpctl` gave up. See
[The link under a long stream](#the-link-under-a-long-stream).

### One row, interrupts off

`vdpctl scenes --seconds 3 --profile --only full-4bpp-32-det`, the maximum of
100 iterations, against Phase 8's table for the same scene on the Pico 2:

| Full mode, 4bpp, 32 magnified sprites, detailed collision | Pico 2 | PRO |
|---|--:|--:|
| Sprite evaluation, 64 slots | 1,821 | 1,852 |
| Layer 0 | 3,307 | 3,196 |
| Layer 1, merged | 5,115 | 4,626 |
| A whole row on one core | 25,221 | 24,806 |
| Left half | 14,244 | 14,018 |
| Right half | 13,904 | 13,715 |
| Expansion, 320 px | 1,959 | 1,962 |
| Choosing the split | 523 | 548 |

Two of these move by more than the scenes do, because one row of a scrolling
scene is not the same row twice: the profile is taken at whatever
scroll position the scene has reached, and which cells row 101 covers decides
how much of layer 1 is transparent. The 36-scene comparison below is the one to
read for the board.

### The PRO against the Pico 2

The same 36 scenes, scene for scene, against draft 0.5's run on the Pico 2:

| Run | Mean difference in the worst line | Largest |
|---|--:|--:|
| 5 s each, no loads | +79 cycles | +590 |
| 5 s each, `FONT` loads both layers every frame | +77 cycles | +649 |
| 16.7 s each, loads and streaming | +55 cycles | +401 |

On a line of 22,372 cycles and a worst line of about 17,000, that is **0.4%,
always in the same direction**. The RP2354A's package, its 2 MB of in-package
flash and the PRO's board make no difference worth the name to a `copy_to_ram`
image: the budget Phase 8 measured is the budget on the card.

### Where the layout does show: the latch under a font load

One number is not within 0.4%. The latch interrupt's worst case with `FONT`
written for both layers every frame — the 2 KB copies §7 puts in the vertical
blank's line start — reads **4,095 cycles** on the PRO against the Pico 2's
3,142–3,246.

That is not the board. Flashing the **`pico2` image onto the PRO** and running
the same scenes gives **3,143**:

| Image | Board | Latch interrupt, `FONT` loads both layers |
|---|---|--:|
| `pico2` | Raspberry Pi Pico 2 (draft 0.5) | 3,142–3,246 |
| `pico2` | **PICO9918 PRO** | 3,143–3,149 |
| `pro-debug` | PICO9918 PRO | 4,095–4,097 |

Same C, same clock, same silicon, same scenes; the two images differ only in
which board header configures them. The most likely reason is where the linker
puts things: a `copy_to_ram` image runs and copies out of the same striped SRAM
the VGA DMA is reading a row from, and moving code and data across banks changes
how often the two collide. It was not chased further here.

It costs nothing yet — 4,095 cycles is 18% of a line and no row was late in any
run — but it is a warning for **Phase 13**, which has to measure this number and
put it in §18: it has to be measured on the image that ships, and a number
measured on another build of the same source can be 30% out.

The same shows under streaming, where the latch also has the snapshot's row copy
against it: 4,202–4,205 cycles on the PRO's image against 3,236–3,245 on the
Pico 2's, with 4,034 snapshots taken in the ten minutes where the Pico 2 took
4,410.

### Under the bus stand-in

Phase 1's stand-in, for what Phase 11 will have to fit into: a PWM interrupt on
core 1 every 2 µs doing a data write into the palette window through the real
`vdp_write`. `vdpctl scenes --seconds 5 --bus 500000`:

- **25 of the 36 scenes fit**, with 1% to 24% spare.
- **Eleven make late rows** — Phase 8's ten on the Pico 2, plus
  `graphics-8bpp-32-det`, which on the Pico 2 finished its worst line 71 cycles
  inside the budget and here finished it 561 cycles outside, for 8 late rows in
  five seconds. A scene sitting on the line falls off it.
- **Every `SPRLIMIT` 16 scene fits**, with 15% to 24% — Phase 8's 16–24%.

So Phase 8's conclusion stands on the card: at 32 the heaviest scenes have no
room for a real bus handler, and at 16 they have about a fifth of the line.

### The stand-in's own race, found here

The first bus run had one more late scene than that: `full-4bpp-16-lim16`, with
9,746 late rows and a worst line of 46,179 — twice the budget, on the *lightest*
of the four `SPRLIMIT` 16 scenes, which had 42% spare without the stand-in. Run
on its own it was fine: 24% spare, three times over.

The stand-in writes through `vdp_write` from an interrupt above the renderer's
thread on core 1, and the scene's own program writes through `vdp_write` from
that thread. They are two producers on the journal the render side consumes, and
nothing kept them apart: the interrupt landing inside the thread's write loses
one of the scene's setup writes, and the scene then runs with the wrong settings
for as long as it runs — which is what 9,746 late rows over five seconds is.

`scene_line` now holds the stand-in off for the scene's program (a few hundred
microseconds at a scene change, a handful of writes a frame after that), and the
outlier is gone. The numbers above are all from the guarded build.

This is a fault in the measuring apparatus, not in the card — but it is the
exact shape of the problem Phase 11 has to solve for real, with a bus handler
that is a second producer at interrupt priority against a render side that is
not expecting one.

### The link under a long stream

The one thing in this phase that did not do what Phase 8 did on the Pico 2.

`vdpctl scenes --seconds 600 --stream` on a single scene ends, three times out
of three, with `no answer to command 3 in 5000 ms` — a `SNAPSHOT` request the
board did not answer within five seconds. One attempt reached 9 minutes 24
seconds and another 3 minutes 47; the first was not timed. The same command for
120 seconds is
clean (800 snapshots), and the 36-scene run, which is the same ten minutes of
streaming broken into 17-second pieces with a scene change between them, is
clean for all 605 seconds and 4,034 snapshots.

**The card is not what fails.** Its own statistics for the 9 minutes 24 seconds
are 8,499,066 rows, no late row, no merged latch, no missed bell, no journal
overflow and no raster slip; `INFO` and `STATS` answered immediately afterwards,
with no fault record and no reset. Only the one request went missing.

The likely mechanism, not chased further here: the link writes through
`stdio_put_string`, and `pico_stdio_usb` **discards** what it cannot hand to the
host within `PICO_STDIO_USB_STDOUT_TIMEOUT_US`. A snapshot is 77 KB, and the
stream runs at about 380 KB a second for ten minutes; one host-side pause long
enough to fill the CDC buffer drops bytes out of the middle of a packet, and the
host then waits for a length that will never arrive. It recovers on the next
request, which is exactly what is seen.

That makes it the debug link's problem and not the renderer's, and it has to be
fixed before **Phase 13**, whose load test streams snapshots for thirty minutes.
The fix is for the link not to drop — a write path that blocks or backpressures
rather than discarding — with the host resynchronising on the packet's sync
bytes as a second line of defence.

---

A late line on purpose
----------------------

`vdpctl late`: scene `full-4bpp-16-lim16`, with LOAD's handicap padding the
build of rows 100, 104, … 128 to 30,000 cycles — 34% past the budget — for 15
seconds with snapshots streaming.

| Check | Result |
|---|---|
| late rows counted | **10,849** of 217,072 rows built. No latch merged |
| rows late in snapshots | 1,344, in 112 snapshots: rows 100, 101, 104, 105, 108, 109, 112, 113, 116, 120, 124 and 128 — the padded rows, and the row after each of the first few, whose build started late |
| each late row shows the most recently finished row | **every one**: 0 rows differ from the host's drawing of the row they show |
| status unaffected | the scene's program read `STAT1`, `STAT7`, `STAT8`–`STAT15` and `STAT0` at screen line 250 of frames 101–1004; **all 904 frames match** `vdp-scene --reads` |

§18's late line, on the card it was specified for.

---

Faults and reflashing
---------------------

Each kind raised from normal mode by `vdpctl fault`, read back from safe mode,
then `vdpctl flash` — with no hands on the board at any point
(`docs/results/phase-10/faults.txt`):

| `FAULT` | Record read from safe mode | Reset reason | Reflash |
|---|---|---|---|
| `core0` | HardFault on core 0, `CFSR` `$00010000` (UNDEFINSTR), PC `fault_raise` at `fault.c:223`, LR `dispatch` at `link.c:365` | fault | normal, no record |
| `core1` | HardFault on core 1, UNDEFINSTR, PC `fault_raise`, LR `apply_request` at `renderer.c:746` | fault | normal, no record |
| `hang` | watchdog: "core 1's renderer stalled" | watchdog | normal, no record |
| `panic` | panic on core 0: "FAULT raised a panic on core 0" | fault | normal, no record |

In safe mode every row is dark red, the renderer is off, and `INFO`, `REBOOT`
and `FAULT` answer. The records are symbolised against the `pro-debug` ELF,
which is what `PICOVDP_PRESET` now selects.

**The BOOT button was pressed exactly once in this phase** — for the first
flash, which replaced the stock firmware. Every flash after that, including the
four here and the one that carried the stand-in guard, went through the SDK's
reset interface with the board in its socket.

---

Done when
---------

PLAN.md's Phase 10:

| Check | Result |
|---|---|
| Phase 8's criteria hold on the PRO — every checkpoint by injection | ✅ all eighteen exact: frame, VRAM, registers, `STAT0`, every read, every `/INT` |
| … the worst-case scenes for ten minutes with zero late lines at `SPRLIMIT` 32, snapshots streaming | ✅ all 36 for ten minutes between them: 8,702,674 rows, none late, 4,034 snapshots, 0 rows wrong. The harshest alone ran its own ten minutes — 8,631,137 rows, none late — and reached 9 min 24 s of that with snapshots streaming before the link, not the card, gave up ([above](#the-link-under-a-long-stream)) |
| … measured cycles recorded against Phase 1 and §18 | ✅ [above](#the-pro-against-the-pico-2); the PRO is the Pico 2 to 0.4% of a line, so §18's budget stands unchanged |
| … a deliberately late line shows its predecessor, status untouched, and is counted | ✅ 10,849 counted; every late row in 112 snapshots is the row before it; 904 frames of status reads match the host |
| … `FAULT`, the record from safe mode, and a reflash with no hands on the board | ✅ HardFaults on both cores, a panic and a watchdog hang; BOOT pressed once in the phase, for the first flash |
| each checkpoint's captured picture matches its golden within the capture tolerance | ✅ all eighteen, and both bench cards. The tolerance is [measured and written down](#what-the-path-does) |
| … which proves the DAC's `0x0BGR` bit order on GPIO 2–13 | ✅ sixteen rising levels a channel with the other two channels never above 0.2 of 255, and every wrong mapping beaten on every picture |
| … line doubling | ✅ two capture rows per card line, identical, across the whole stripe band |
| … palette expansion | ✅ all 256 of §11's entries measured on their own swatches, every ramp monotonic |
| a 256-entry palette test card is captured for the owner to judge the hue ramps | ✅ `shots/palette-palette-capture.png`; the judgement is the owner's |
| the host suite passes, the firmware presets build | ✅ 27 tests and 20 under ASan and UBSan; `pico2`, `pro-debug` and `pro-release` build with no warnings |

---

Differences from the plan
-------------------------

1. **The firmware needed nothing.** PLAN.md expected this phase to be Phase 8
   again on different hardware, and it was: the `pro-debug` image is the `pico2`
   image with another board header, and it ran first time. The only firmware
   change in the phase is the stand-in guard, which is in debug builds only and
   is about the measuring apparatus, not the card.
2. **The capture comparison is a method, not a threshold.** The plan says
   "matches its golden within the capture tolerance" and left the tolerance to
   be found. It turns out there is no single per-pixel tolerance that an analog
   path can meet on every picture: the oracle's densest frames have no pixel
   that ever settles, and the saturated primaries of the DAC card are where the
   path's own curve is worst. So the tolerance is four numbers over three kinds
   of pixel, and `tools/lib/screen.mjs` states each of them and what was
   measured for it.
3. **Two bench cards, not one.** The plan asked for the palette card. The DAC
   card was added because it is what turns "the bit order is right" from an
   opinion about a picture into sixteen measured levels a channel with the
   other two channels dark. It also measures the path's response, which is what
   makes the palette card's result meaningful.
4. **The cards are this repository's own goldens.** They are drawn by the
   reference, checked against the core, and kept in `bench/cards/` — not in
   `tests/oracle/`, which belongs to `tools/sync-oracle.mjs` alone (ground rule
   4). CTest `cards_pinned` holds them to the core without needing an emulator.
5. **One firmware change, in the measuring apparatus.** `scene_line` now holds
   the bus stand-in off while the scene's own program writes, because the two
   were both producing into the journal from different priorities on core 1. It
   is behind `PICOVDP_DEBUG` and the card cannot reach it.
6. **No SPEC.md change.** Everything this phase measured either confirms SPEC.md
   (§2's DAC, §3's line doubling, §11's palette, §18's budget) or is a property
   of the bench. Nothing normative moved and no golden changed.

---

For later phases
----------------

- **Phase 11.** The board it lands on is now known good: the picture, the
  clocks, the DAC, the link and the flash path all work with nothing attached to
  the bus, so anything that breaks when the PIO programs go in is the bus. The
  budget is the Pico 2's, so Phase 8's conclusion stands unchanged — at
  `SPRLIMIT` 32 the heaviest scenes have no room for a real bus handler, and at
  16 they have 16–24%. The stand-in's race (above) is the shape of the problem
  Phase 11 has to solve properly: the bus handler is a second producer on the
  journal, at interrupt priority, against a render side that is not expecting
  one.
- **Phase 12.** Every checkpoint now reproduces by injection *and* on the
  monitor. What Phase 12 adds is the same traces through the Nano, so the
  capture comparison and its tolerance carry over unchanged — `inject --capture`
  and `vdpctl card` work against any trace with a golden beside it.
- **Phase 13.** `docs/results/phase-10/profile-harshest.json` has the stage
  cycles on the PRO for §18's table to be re-measured against under bus load,
  and [the latch under a font load](#where-the-layout-does-show-the-latch-under-a-font-load)
  says to measure on the image that ships rather than any build of the source.
  The bench cards are also the obvious load test: a card is a static picture
  whose every colour is known, so a capture under load is a picture that either
  is or is not still right.
- **The debug link.** `stdio_put_string` drops what the host cannot take in
  time, and over a ten-minute stream it does
  ([above](#the-link-under-a-long-stream)). Phase 13 streams for thirty minutes;
  this is on its path.
- **The owner.** `shots/palette-palette-capture.png` is waiting on the hue-ramp
  judgement (Still Open 3).

---

Reproducing
-----------

The bench is `docs/BENCH.md`: the PRO on its breadboard, the FFC to the HDMI
dongle, the dongle to the capture card. The Nano takes no part in this phase.

```sh
cmake --workflow --preset host          # 27 tests, cards_pinned among them
cmake --preset pro-debug && cmake --build --preset pro-debug
export PICOVDP_PRESET=pro-debug         # which image flash sends, which ELF a fault is read against
node tools/vdpctl.mjs flash             # the board in BOOTSEL for this one flash only

node tools/vdpctl.mjs inject all --capture --pictures shots --out inject-capture.json
node tools/vdpctl.mjs card dac --out card-dac.json --pictures shots
node tools/vdpctl.mjs card palette --out card-palette.json --pictures shots

node tools/vdpctl.mjs scenes --seconds 5 --out scenes-without-loads.json
node tools/vdpctl.mjs scenes --seconds 5 --fonts --out scenes-with-loads.json
node tools/vdpctl.mjs scenes --seconds 5 --bus 500000 --out scenes-bus.json
node tools/vdpctl.mjs scenes --seconds 16.7 --fonts --stream --out scenes-10min-all.json
node tools/vdpctl.mjs scenes --seconds 600 --fonts --only full-4bpp-32-det
node tools/vdpctl.mjs scenes --seconds 3 --profile --only full-4bpp-32-det --out profile-harshest.json
node tools/vdpctl.mjs late --seconds 15
for f in core0 core1 hang panic; do node tools/vdpctl.mjs fault $f; node tools/vdpctl.mjs flash; done

node tools/card.mjs all --png           # redraw the bench cards from the reference
```

The last one needs a 6502-EMULATOR checkout with `npm run build:cli` run in it;
everything else does not. Raw results are in `docs/results/phase-10/`.
