Phase 8 — Firmware on the Pico 2
================================

**Status:** done on 2026-09-14. Every check below passed.

**The headline.**

- **The goldens reproduce on silicon.** All fifteen checkpoints were replayed
  on a Raspberry Pi Pico 2 by injection. The index frame as it went to VGA, all
  64 KB of VRAM, the 128 registers and `STAT0` are exact, and so are every read and
  every `/INT` level the traces carry: 4,119,098 operations, 4,005,057 of them
  reads.
- **No late line in ten minutes.** Phase 1's 36 worst-case scenes, rebuilt as
  port writes and drawn by the real core on both cores, ran ten minutes between
  them with snapshots streaming over USB throughout. 8,681,495 rows, none late;
  4,389 snapshots, every row exact against the host's drawing of the same frame.
  The harshest scene — Full mode, 4bpp, 32 magnified 16 × 16 sprites with
  detailed collision — then ran ten minutes alone: 8,632,935 rows, none late, the
  worst line 20,216 cycles against a budget of 22,372, 4,057 snapshots exact.
- **A late line does what §18 says.** With rows padded past the budget on
  purpose, 10,860 late rows were counted. 1,308 of them were caught in snapshots,
  and each showed the row before it. What the scene's program read each frame —
  `STAT0`, `STAT1`, `STAT7` and the collision map — matched the host's in all 906
  frames compared.
- **A crash is recovered over USB.** A HardFault on either core, a panic, and a
  hang caught by the watchdog each left a fault record, symbolised by `vdpctl`,
  read from safe mode; `vdpctl flash` then brought the board back, with no hands
  on it.
- **The core had to get faster, and did.** Phase 7's renderer, drawn a pixel at
  a time, took about three times the line on silicon: every row was late. The
  layers and sprites were rewritten a word at a time, as Phase 1's spike drew
  them, and each row is now built in two halves, one a core, with no merge. Held
  to Phase 7's renderer (kept as a reference) over 25,500 random lines at ten
  splits, to `Video.ts` over 109,807 fuzzed frames, to all 342 Jest tests and the
  fifteen goldens on the host.
- **The bus will be tight.** Under Phase 1's bus stand-in, a VRAM write every 2
  µs on core 1, `SPRLIMIT` 16 fits every case with 16–24% spare, not the 25% draft
  0.4 recorded from the spike. At 32, ten of the 36 scenes make late lines. Phase
  11 measures the real handler.

Contents: [What was built](#what-was-built) · [The renderer, a word at a time](#the-renderer-a-word-at-a-time) ·
[The firmware](#the-firmware) · [The goldens on silicon](#the-goldens-on-silicon) ·
[The worst-case scenes](#the-worst-case-scenes) · [A late line on purpose](#a-late-line-on-purpose) ·
[Faults, the watchdog and safe mode](#faults-the-watchdog-and-safe-mode) · [On the host](#on-the-host) ·
[Done when](#done-when) · [Differences from the plan](#differences-from-the-plan) ·
[For later phases](#for-later-phases) · [Reproducing](#reproducing)

---

What was built
--------------

| Part | What it is |
|---|---|
| `firmware/main.c` | The clock (352 MHz, 1.30 V), USB, safe mode or the renderer, the watchdog, the link |
| `firmware/vga/` | pico9918's driver cut to VGA 640 × 480, raising a PIO interrupt at each of §3's 262 line starts; unchanged `vga.pio` |
| `firmware/renderer.c` | The card on two cores: core 1's latch interrupt and renderer, core 0's half of each row, the row buffers and late lines. Debug builds: statistics, snapshots, requests from the link, scenes, the bus stand-in and the handicap |
| `firmware/fault.c` | Fault records, the HardFault handler on both cores, `PICO_PANIC_FUNCTION`, the watchdog and its feeder, safe mode |
| `firmware/link.c` | The debug link (`docs/DEBUGLINK.md`), debug builds |
| `firmware/inject.c` | The injection executor, debug builds |
| `firmware/scenes.c` | Phase 1's worst-case scenes as port writes, portable C: the firmware and `vdp-scene` build the same scenes |
| `firmware/profile.c` | PROFILE: one row's stages timed with interrupts off |
| `core/` | The latch in halves (`vdp_latch`, `vdp_catch_up`, `vdp_publish`); layers and sprites a word at a time (`tiles.c`, `sprites.c`, `tables.c`); a row in halves (`vdp_build_half`, `vdp_expand_half`, `vdp_copy_half`); a faster evaluation; the split from a cost-weighted mean |
| `host/scene/` | `vdp-scene`: the scenes drawn by the core on the host, their reads, and a check that each is the worst case it claims (CTest `vdp_scene_check`) |
| `tools/vdpctl.mjs`, `tools/lib/link.mjs`, `inject.mjs`, `scenes.mjs` | `flash`, `info`, `stats`, `snapshot`, `vram`, `inject`, `reset`, `reboot`, `fault`, `scene`, `load`, `scene-log`, `profile`, `scenes`, `late`. No npm dependency: the port is a tty |
| `tests/unit/test_render.c`, `reference.c` | Phase 7's pixel renderer kept as a reference; the word renderer held to it |
| `tests/unit/test_latch.c` | Four new tests of the latch in halves |
| `docs/DEBUGLINK.md` | The link, specified |

**Memory.** The `pico2` image is 107 KB of code and 381 KB of data in the RP2350's
512 KB of SRAM: the card (139 KB), a snapshot's frame (77 KB), a copy of VRAM
(64 KB), the injection ring (32 KB), four row buffers and core 0's half. `pro-release`
is 81 KB and 170 KB. All three presets build with no warnings.

**The RP2354A.** The PRO's MCU is an RP2354A, an RP2350A with 2 MB of flash in
the package; the Pico 2 has an RP2350A and 4 MB beside it. Nothing here depends
on the difference: every preset targets `rp2350-arm-s`, `pico9918pro.h` declares
2 MB, and a `copy_to_ram` image of about 200 KB never touches flash after boot.
PLAN.md, README.md and SPEC.md §2 now name it.

---

The renderer, a word at a time
------------------------------

**What was wrong.** The first image ran Phase 7's renderer as it was. On the
harshest scene it took 34,705 cycles for core 1's layers and 56,365 for core 0's
sprites, where Phase 1's spike had taken about 7,900 and 14,700: every row was
late, and the latch ring merged. Phase 7 had warned of this and named the fallback.

**What changed.**

1. **Layers.** An 8-pixel cell is two words of indices and two of masks, decoded
   by table (4bpp), by value table (2bpp) or loaded (8bpp); flips are byte swaps
   of the decoded words; the level contest is a word comparison. Cells are drawn
   whole into a line with 8 bytes of slack, and the border is put back after. One
   copy of the loop for each depth, attribute source, level mode and index-0
   opacity. Text's 6-pixel cells keep Phase 7's pixel loop.
2. **Sprites.** Each sprite's row is decoded once into indices and a solid mask,
   flipped and magnified in words. It claims pixels in bitmaps: collision is a
   word AND, and only the nibbles it newly takes are drawn. Legacy colour 0
   sprites, which cover without painting, get a second bitmap. Detailed collision
   records, per claim word, which slot first covered which pixels.
3. **Evaluation.** One loop for the legacy reading and one for `VMODE`'s, with
   few live values: 2,909 cycles became 1,817.
4. **A row in two halves.** Phase 1's shape had core 0 draw its sprites into a
   sprite line that core 1 merged after building both layers. Measured here,
   the merge cost 3,300 cycles and the whole shape left 9% spare. Now each core
   builds both layers and the sprites over its own columns, into a line of its
   own, and expands its columns into the row's buffer: nothing in a column depends
   on another, so the halves are exact, and there is no merge. That left 12%, and
   rows without sprites are halved too.
5. **The split.** A cost-weighted mean column over both layers, the expansion and
   each sprite at its centre, on an 8-pixel boundary, then moved 8 pixels a row
   toward the core that finished first. The bucket walk tried first cost 1,071
   cycles; this costs about 510.

**How the halves went on each harshest-scene measurement,** no bus:

| Harshest scene, no bus | Worst line | Spare |
|---|--:|--:|
| Phase 7's renderer | every row late | — |
| Words, Phase 1's merge shape | 22,005 | 2% |
| … 8-pixel splits, drawing only fresh nibbles | 20,347 | 9% |
| Halves, whole buffers cleared, bucket split | 21,231 | 5% |
| **Halves, own columns cleared, weighted-mean split** | **19,648** | **12%** |
| The same, ten minutes, snapshots streaming | 20,216 | 10% |

**One row, interrupts off** (`vdpctl profile --row 101`, the maximum of three
runs of 100 iterations), against Phase 1's stage suite and §18's table:

| Full mode, 4bpp | §18 estimate | Phase 1 spike | Phase 8 core |
|---|--:|--:|--:|
| Sprite evaluation, 64 slots, list full | ~500 | 1,258 | 1,817 |
| Layer 0 | ~800 | 2,846 | 3,312 |
| Layer 1, merged | ~1,600 | 5,021 | 5,118 |
| 32 × 16 × 16 sprites | ~2,600 | 10,087 | ≈ 11,500 |
| … magnified, detailed collision | ~5,700 | 15,850 | ≈ 16,800 |
| Expansion, 320 px | ~1,300 | 1,662 | 1,948 |

The sprite rows are the whole row on one core less both layers; they include the
line's clearing. Layers, sprites and expansion are within 2–17% of the spike's,
while drawing the whole of SPEC.md — the legacy submode, every attribute source,
Text, priority among seven levels — which the spike did not. Evaluation is 44%
dearer, though it stops at the 33rd slot: it fills the list the build needs.

Every scene, one row, interrupts off (cycles):

| Scene | Sprites | Evaluate | Layer 0 | Layer 1 | Row, one core | Left half | Right half | Expand | Split choice |
|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| graphics-1bpp-16 | 32 | 1,825 | 2,141 | 3,703 | 14,302 | 8,650 | 7,792 | 1,956 | 510 |
| graphics-1bpp-16-det | 32 | 1,819 | 2,315 | 3,920 | 17,977 | 10,771 | 9,658 | 1,945 | 515 |
| graphics-1bpp-32 | 32 | 1,825 | 2,137 | 3,703 | 16,402 | 9,784 | 9,560 | 1,964 | 512 |
| graphics-1bpp-32-det | 32 | 1,819 | 2,141 | 3,697 | 19,495 | 11,497 | 11,309 | 1,958 | 516 |
| graphics-2bpp-16 | 32 | 1,819 | 2,647 | 4,345 | 17,619 | 10,474 | 9,460 | 1,936 | 509 |
| graphics-2bpp-16-det | 32 | 1,825 | 2,650 | 4,341 | 20,736 | 12,298 | 11,014 | 1,937 | 521 |
| graphics-2bpp-32 | 32 | 1,819 | 2,650 | 4,337 | 19,722 | 11,601 | 11,427 | 1,948 | 511 |
| graphics-2bpp-32-det | 32 | 1,825 | 2,650 | 4,334 | 22,721 | 13,290 | 13,147 | 1,946 | 520 |
| graphics-4bpp-16 | 32 | 1,825 | 2,732 | 4,169 | 17,466 | 10,344 | 9,342 | 1,937 | 513 |
| graphics-4bpp-16-det | 32 | 1,825 | 2,732 | 4,170 | 20,651 | 12,222 | 10,895 | 1,940 | 516 |
| graphics-4bpp-32 | 32 | 1,825 | 2,731 | 4,167 | 19,582 | 11,487 | 11,293 | 1,956 | 513 |
| graphics-4bpp-32-det | 32 | 1,819 | 2,929 | 4,234 | 23,114 | 13,484 | 13,303 | 1,951 | 519 |
| graphics-8bpp-16 | 32 | 1,819 | 2,176 | 3,889 | 16,174 | 9,680 | 8,711 | 1,943 | 515 |
| graphics-8bpp-16-det | 32 | 1,817 | 2,038 | 3,712 | 19,033 | 11,406 | 10,130 | 1,933 | 515 |
| graphics-8bpp-32 | 32 | 1,825 | 2,038 | 3,706 | 17,747 | 10,567 | 10,330 | 1,955 | 510 |
| graphics-8bpp-32-det | 32 | 1,817 | 2,034 | 3,706 | 21,000 | 12,362 | 12,169 | 1,959 | 514 |
| full-1bpp-16 | 32 | 1,815 | 2,544 | 4,538 | 16,471 | 9,597 | 8,857 | 1,973 | 508 |
| full-1bpp-16-det | 32 | 1,817 | 2,782 | 4,848 | 20,386 | 11,845 | 10,820 | 1,958 | 514 |
| full-1bpp-32 | 32 | 1,817 | 2,543 | 4,536 | 18,563 | 10,707 | 10,376 | 1,963 | 508 |
| full-1bpp-32-det | 32 | 1,816 | 2,773 | 4,858 | 22,564 | 12,882 | 12,560 | 1,963 | 514 |
| full-2bpp-16 | 32 | 1,819 | 3,168 | 5,318 | 20,094 | 11,493 | 10,651 | 1,960 | 509 |
| full-2bpp-16-det | 32 | 1,819 | 3,167 | 5,325 | 23,292 | 13,338 | 12,234 | 1,959 | 519 |
| full-2bpp-32 | 32 | 1,817 | 3,175 | 5,326 | 22,168 | 12,576 | 12,317 | 1,964 | 505 |
| full-2bpp-32-det | 32 | 1,816 | 3,167 | 5,319 | 25,305 | 14,286 | 14,026 | 1,972 | 518 |
| full-4bpp-16 | 32 | 1,817 | 3,312 | 5,118 | 19,926 | 11,373 | 10,506 | 1,948 | 506 |
| full-4bpp-16-det | 32 | 1,818 | 3,313 | 5,116 | 23,102 | 13,247 | 12,106 | 1,941 | 519 |
| full-4bpp-32 | 32 | 1,816 | 3,534 | 5,227 | 22,575 | 12,811 | 12,508 | 1,956 | 505 |
| full-4bpp-32-det | 32 | 1,821 | 3,307 | 5,115 | 25,221 | 14,244 | 13,904 | 1,959 | 523 |
| full-8bpp-16 | 32 | 1,817 | 2,696 | 4,782 | 18,469 | 10,670 | 9,844 | 1,946 | 512 |
| full-8bpp-16-det | 32 | 1,817 | 2,697 | 4,779 | 21,918 | 12,631 | 11,521 | 1,947 | 513 |
| full-8bpp-32 | 32 | 1,816 | 2,417 | 4,549 | 19,869 | 11,412 | 11,084 | 1,962 | 513 |
| full-8bpp-32-det | 32 | 1,813 | 2,416 | 4,538 | 23,271 | 13,249 | 12,925 | 1,958 | 520 |
| full-4bpp-16-lim16 | 16 | 989 | 3,305 | 5,114 | 14,830 | 9,180 | 7,369 | 1,941 | 316 |
| full-4bpp-16-det-lim16 | 16 | 991 | 3,504 | 5,346 | 17,308 | 10,829 | 8,496 | 1,945 | 320 |
| full-4bpp-32-lim16 | 16 | 987 | 3,303 | 5,116 | 16,403 | 9,210 | 9,201 | 1,942 | 315 |
| full-4bpp-32-det-lim16 | 16 | 991 | 3,313 | 5,116 | 18,297 | 10,260 | 10,305 | 1,946 | 320 |

"Row, one core" is `vdp_build_half` over the whole picture; "left" and "right"
are the halves at the picture's middle, and cost about 2,500 cycles more between
them than the whole, for clearing and for sprites decoded on both sides of the
split. Two early profile passes returned, for two scenes, figures identical to the
scene before; the passes above settle each scene for two seconds, take three
readings and check none repeats the scene before. The stale answers did not
recur, and are not explained.

---

The firmware
------------

**The raster.** pico9918's sync program runs a 525-line table built once. The back
porch before each screen line's first VGA line raises PIO interrupt 0, and before
screen line 0 interrupt 1, so every line start is on time and numbered from the
program's own flags. At 352 MHz the PIO divider is 7 and a display line is 2 ×
1,598 ticks: **22,372 cycles**. The raster ran at 59.94 frames a second, 14,380
rows a second.

**The line start** (core 0, highest). It records the line's frame and screen line
for core 1, rings core 1's doorbell, and starts the row's RGB DMA with the latest
finished row, whichever row that is: a late row sends its predecessor, for both
VGA lines. Its worst case was 546 cycles, with a snapshot copying its row.

**The latch** (core 1, highest). `vdp_latch` for every line begun since the last,
tagged with the line's event number. Worst case 357 cycles.

**The renderer** (core 1's thread). For each latch it catches up with: choose the
split, post it to core 0, build and expand core 1's half, wait for core 0's,
publish with interrupts held off, and mark the row's buffer finished. It builds
into one of four buffers that is neither being sent nor among the last two
finished, so a finished row can wait while another is sent and a third built.

**Core 0's half** runs in its inter-core FIFO interrupt, at `$40`: below VGA and
above USB, so a blocking USB write never delays a build.

**Start-up.** Core 1 is launched with its own 8 KB stack, takes its handshake off
the FIFO, installs its latch and says so; only then does core 0 enable its FIFO
interrupt and start the raster (PLAN.md section 3's fix).

**Status** is published when core 1 finishes the row, both halves' collisions
and the evaluation's overflow together. Through `vdp_line_start` and
`vdp_build_line` on the host that is at the latch and in the build, as
`Video.ts` does.

---

The goldens on silicon
----------------------

`vdpctl inject all`: each checkpoint replayed from its trace's cold reset, the
golden frame captured as it went to VGA, VRAM read at the checkpoint, registers
and `STAT0` recorded there.

| Checkpoint | Frame | Operations | Reads | Stream | Time | Result |
|---|--:|--:|--:|--:|--:|---|
| `bios/ok` | 419 | 4,230 | 1 | 13.6 KB | 7.5 s | **exact** |
| `bios/screenful` | 639 | 4,623 | 1 | 15.4 KB | 11.1 s | **exact** |
| `bios/scroll` | 840 | 12,597 | 3,681 | 43.4 KB | 14.5 s | **exact** |
| `wizardslab/frame-60` | 59 | 92,373 | 86,224 | 267 KB | 1.5 s | **exact** |
| `wizardslab/frame-180` | 179 | 294,440 | 288,255 | 844 KB | 3.5 s | **exact** |
| `wizardslab/frame-300` | 299 | 496,322 | 490,083 | 1.42 MB | 5.6 s | **exact** |
| `wizardslab/frame-600` | 599 | 1,001,098 | 994,751 | 2.86 MB | 10.8 s | **exact** |
| `vdp-modes/text` | 35 | 59,665 | 53,591 | 176 KB | 1.1 s | **exact** |
| `vdp-modes/compact` | 98 | 168,677 | 160,985 | 493 KB | 2.2 s | **exact** |
| `vdp-modes/graphics` | 162 | 279,275 | 269,547 | 814 KB | 3.3 s | **exact** |
| `vdp-modes/full` | 226 | 390,489 | 378,521 | 1.14 MB | 4.3 s | **exact** |
| `vdp-layers/parallax` | 89 | 122,313 | 114,465 | 375 KB | 2.0 s | **exact** |
| `vdp-layers/scroll-bit8-l1` | 179 | 287,521 | 278,773 | 847 KB | 3.6 s | **exact** |
| `vdp-layers/occluded` | 239 | 397,669 | 388,321 | 1.16 MB | 4.6 s | **exact** |
| `vdp-layers/scroll-bit8-l0` | 299 | 507,806 | 497,858 | 1.48 MB | 5.6 s | **exact** |

Exact means: the 76,800 indices of the golden frame, with no late row in it; the
64 KB of VRAM; the 128 registers, read through their aliases; `STAT0`; every read
returning the trace's value; `/INT` after every operation and at every line the
trace moved it. No read selected `STAT5`, so none went uncompared.

The first run differed in one register, `$04`, which was the host comparing the
raw file, where `$02`–`$06` live at their homes, with the golden's alias reads.
`vdpctl` now reads them as `vdp-replay` does.

---

The worst-case scenes
---------------------

`firmware/scenes.c` builds each of Phase 1's scenes through the ports: both layers
scrolled with per-cell attributes, flips, a quarter priority, layer 1 half
transparent; 64 slots covering the band at display lines 100 up, `SPRLIMIT` drawn,
every sprite pixel solid, each overlapping the next. At each screen line 250 its
program reads status and scrolls. `vdp-scene --check` confirms on the host that
every scene lists `SPRLIMIT` sprites, overflows and collides on every row of the
band, with every drawn sprite in the detailed map.

**Ten minutes, all 36 scenes**, `vdpctl scenes --seconds 16.7 --stream`. Latency
is from core 1's latch interrupt to the row's buffer finished; spare is against
22,372. Each snapshot was checked against `vdp-scene`'s drawing of the frame its
state named.

| Scene | Seconds | Rows built | Late rows | Latency max | 99.9% | Mean | Spare | Core 1 half max | Core 0 half max | Snapshots | Rows wrong |
|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| graphics-1bpp-16 | 16.8 | 241,623 | 0 | 14,303 | 14,207 | 6,839 | 36% | 9,511 | 10,346 | 122 | 0 |
| graphics-1bpp-16-det | 16.8 | 242,085 | 0 | 16,197 | 15,999 | 6,963 | 28% | 11,255 | 12,204 | 124 | 0 |
| graphics-1bpp-32 | 16.8 | 242,172 | 0 | 15,957 | 15,487 | 7,345 | 29% | 10,654 | 12,084 | 118 | 0 |
| graphics-1bpp-32-det | 16.8 | 242,150 | 0 | 17,909 | 17,791 | 7,641 | 20% | 12,415 | 13,952 | 115 | 0 |
| graphics-2bpp-16 | 16.7 | 240,676 | 0 | 16,050 | 15,999 | 8,191 | 28% | 11,217 | 12,087 | 121 | 0 |
| graphics-2bpp-16-det | 16.7 | 240,691 | 0 | 17,920 | 17,791 | 8,313 | 20% | 13,015 | 13,934 | 121 | 0 |
| graphics-2bpp-32 | 16.8 | 241,172 | 0 | 17,814 | 17,407 | 8,866 | 20% | 12,398 | 13,840 | 114 | 0 |
| graphics-2bpp-32-det | 16.7 | 240,609 | 0 | 19,679 | 19,583 | 9,144 | 12% | 14,173 | 15,696 | 119 | 0 |
| graphics-4bpp-16 | 16.7 | 240,481 | 0 | 15,951 | 15,871 | 8,152 | 29% | 11,175 | 12,058 | 125 | 0 |
| graphics-4bpp-16-det | 16.8 | 241,430 | 0 | 17,832 | 17,791 | 8,273 | 20% | 12,936 | 13,922 | 125 | 0 |
| graphics-4bpp-32 | 16.8 | 242,141 | 0 | 17,719 | 17,407 | 8,836 | 21% | 12,408 | 13,832 | 124 | 0 |
| graphics-4bpp-32-det | 16.8 | 241,383 | 0 | 19,710 | 19,583 | 9,111 | 12% | 14,160 | 15,699 | 120 | 0 |
| graphics-8bpp-16 | 16.8 | 241,200 | 0 | 15,078 | 14,975 | 7,550 | 33% | 10,172 | 11,139 | 125 | 0 |
| graphics-8bpp-16-det | 16.7 | 240,460 | 0 | 17,076 | 17,023 | 7,683 | 24% | 12,069 | 13,113 | 125 | 0 |
| graphics-8bpp-32 | 16.8 | 241,930 | 0 | 16,430 | 16,255 | 8,201 | 27% | 11,361 | 12,787 | 126 | 0 |
| graphics-8bpp-32-det | 16.7 | 240,942 | 0 | 18,759 | 18,687 | 8,480 | 16% | 13,207 | 14,765 | 125 | 0 |
| full-1bpp-16 | 16.7 | 240,751 | 0 | 15,395 | 15,231 | 7,274 | 31% | 10,509 | 11,416 | 125 | 0 |
| full-1bpp-16-det | 16.7 | 240,951 | 0 | 17,204 | 17,023 | 7,450 | 23% | 12,156 | 13,278 | 125 | 0 |
| full-1bpp-32 | 16.8 | 241,440 | 0 | 16,486 | 16,383 | 7,869 | 26% | 11,546 | 12,921 | 125 | 0 |
| full-1bpp-32-det | 16.8 | 241,647 | 0 | 18,641 | 18,175 | 8,107 | 17% | 13,339 | 14,822 | 123 | 0 |
| full-2bpp-16 | 16.8 | 241,899 | 0 | 17,203 | 17,151 | 8,992 | 23% | 12,276 | 13,194 | 125 | 0 |
| full-2bpp-16-det | 16.8 | 241,887 | 0 | 19,061 | 18,943 | 9,116 | 15% | 13,993 | 15,075 | 125 | 0 |
| full-2bpp-32 | 16.7 | 240,457 | 0 | 18,386 | 18,303 | 9,680 | 18% | 13,348 | 14,779 | 116 | 0 |
| full-2bpp-32-det | 16.7 | 240,642 | 0 | 20,471 | 19,967 | 9,917 | 8% | 15,151 | 16,628 | 115 | 0 |
| full-4bpp-16 | 16.7 | 240,454 | 0 | 17,009 | 16,895 | 8,934 | 24% | 12,223 | 13,189 | 125 | 0 |
| full-4bpp-16-det | 16.7 | 240,669 | 0 | 18,921 | 18,815 | 9,060 | 15% | 13,911 | 15,037 | 125 | 0 |
| full-4bpp-32 | 16.7 | 240,714 | 0 | 18,319 | 18,175 | 9,631 | 18% | 13,374 | 14,652 | 124 | 0 |
| full-4bpp-32-det | 16.7 | 240,906 | 0 | 20,164 | 19,967 | 9,866 | 10% | 15,115 | 16,582 | 121 | 0 |
| full-8bpp-16 | 16.7 | 240,706 | 0 | 16,000 | 15,871 | 8,211 | 28% | 11,079 | 12,101 | 125 | 0 |
| full-8bpp-16-det | 16.7 | 240,720 | 0 | 17,943 | 17,663 | 8,345 | 20% | 12,917 | 14,096 | 125 | 0 |
| full-8bpp-32 | 16.7 | 240,715 | 0 | 17,094 | 16,767 | 8,873 | 24% | 12,179 | 13,147 | 125 | 0 |
| full-8bpp-32-det | 16.8 | 241,375 | 0 | 19,283 | 18,943 | 9,130 | 14% | 14,081 | 15,651 | 124 | 0 |
| full-4bpp-16-lim16 | 16.8 | 241,030 | 0 | 13,368 | 12,927 | 8,679 | 40% | 9,114 | 10,523 | 125 | 0 |
| full-4bpp-16-det-lim16 | 16.7 | 240,747 | 0 | 14,232 | 14,079 | 8,755 | 36% | 9,993 | 11,368 | 116 | 0 |
| full-4bpp-32-lim16 | 16.8 | 241,923 | 0 | 14,305 | 13,951 | 9,057 | 36% | 10,187 | 11,350 | 113 | 0 |
| full-4bpp-32-det-lim16 | 16.7 | 240,717 | 0 | 15,470 | 15,359 | 9,220 | 31% | 11,254 | 12,490 | 113 | 0 |

Total: 8,681,495 rows, 0 late, 0 merged, 4389 snapshots, 0 rows wrong, 603 s

**Ten minutes, the harshest alone**, `vdpctl scenes --minutes 10 --stream --only
full-4bpp-32-det`: 8,632,935 rows, 0 late, 0 merged; latency max 20,216 (10%
spare), 99.9th percentile 20,095, mean 9,862; core 1's half at most 15,129, core
0's 16,603; catch-up at most 2,523, publish 819; 4,057 snapshots, every row exact.
No missed bells, journal overflows or raster slips. The board had then been up
21 minutes at 1.30 V.

**Against Phase 1 and §18,** Full mode, 4bpp, the longest line from latch to
expanded output:

| Full mode, 4bpp | Phase 1, bus stand-in | Phase 8, no bus | Phase 8, bus stand-in |
|---|--:|--:|--:|
| 16 × 16 | 19,448 | 17,009 | 21,005 |
| 16 × 16, detailed | 20,255 | 18,921 | 27,885, late |
| Magnified | 20,962 | 18,319 | 23,009, late |
| Magnified, detailed | 21,850 | 20,164 | 80,364, late |
| 16 × 16, `SPRLIMIT` 16 | 16,475 | 13,368 | 16,990 |
| 16 × 16, detailed, `SPRLIMIT` 16 | 16,501 | 14,232 | 18,025 |
| Magnified, `SPRLIMIT` 16 | 16,800 | 14,305 | 17,667 |
| Magnified, detailed, `SPRLIMIT` 16 | 16,862 | 15,470 | 18,874 |

**Under the bus stand-in** (`vdpctl scenes --seconds 5 --bus 500000`, no
streaming). The stand-in is Phase 1's: a PWM interrupt on core 1 every 2 µs doing
a data write into the palette window through the real `vdp_write`, so each
catch-up drains 32 palette entries. It costs core 1 about 4,000 cycles a line.

| Scene | Late rows | Latency max | 99.9% | Spare |
|---|--:|--:|--:|--:|
| graphics-1bpp-16 | 0 | 17,993 | 17,919 | 20% |
| graphics-1bpp-16-det | 0 | 19,924 | 19,839 | 11% |
| graphics-1bpp-32 | 0 | 19,672 | 19,455 | 12% |
| graphics-1bpp-32-det | 0 | 21,811 | 21,631 | 3% |
| graphics-2bpp-16 | 0 | 19,866 | 19,839 | 11% |
| graphics-2bpp-16-det | 0 | 21,827 | 21,375 | 2% |
| graphics-2bpp-32 | 0 | 21,230 | 21,119 | 5% |
| graphics-2bpp-32-det | 9672 | 36,041 | 34,431 | late |
| graphics-4bpp-16 | 0 | 19,828 | 19,711 | 11% |
| graphics-4bpp-16-det | 0 | 21,713 | 21,247 | 3% |
| graphics-4bpp-32 | 0 | 21,217 | 21,119 | 5% |
| graphics-4bpp-32-det | 9611 | 35,900 | 34,431 | late |
| graphics-8bpp-16 | 0 | 18,775 | 18,687 | 16% |
| graphics-8bpp-16-det | 0 | 20,749 | 20,479 | 7% |
| graphics-8bpp-32 | 0 | 20,137 | 19,967 | 10% |
| graphics-8bpp-32-det | 0 | 22,301 | 22,143 | 0% |
| full-1bpp-16 | 0 | 19,072 | 18,943 | 15% |
| full-1bpp-16-det | 0 | 21,071 | 20,991 | 6% |
| full-1bpp-32 | 0 | 20,661 | 20,479 | 8% |
| full-1bpp-32-det | 237 | 23,261 | 23,039 | late |
| full-2bpp-16 | 0 | 21,039 | 20,991 | 6% |
| full-2bpp-16-det | 3931 | 28,330 | 27,903 | late |
| full-2bpp-32 | 11 | 23,087 | 22,271 | late |
| full-2bpp-32-det | 10792 | 80,260 | 65,535 | late |
| full-4bpp-16 | 0 | 21,005 | 20,863 | 6% |
| full-4bpp-16-det | 3049 | 27,885 | 26,879 | late |
| full-4bpp-32 | 6 | 23,009 | 22,271 | late |
| full-4bpp-32-det | 10711 | 80,364 | 65,535 | late |
| full-8bpp-16 | 0 | 19,836 | 19,711 | 11% |
| full-8bpp-16-det | 0 | 22,056 | 22,015 | 1% |
| full-8bpp-32 | 0 | 21,372 | 21,119 | 4% |
| full-8bpp-32-det | 9634 | 41,865 | 39,167 | late |
| full-4bpp-16-lim16 | 0 | 16,990 | 16,383 | 24% |
| full-4bpp-16-det-lim16 | 0 | 18,025 | 17,663 | 19% |
| full-4bpp-32-lim16 | 0 | 17,667 | 17,663 | 21% |
| full-4bpp-32-det-lim16 | 0 | 18,874 | 18,815 | 16% |

Without the bus the scenes above had 8–40% spare; the stand-in takes 4–10
points of that. Every `SPRLIMIT` 16 scene fits, with 16–24%. At 32, 26 of 36 fit
and ten make late lines: detailed collision or magnification in Full mode at 2 and
4bpp, and both together in Full mode at 1 and 8bpp and in Graphics mode at 2 and
4bpp. No latch was merged even then: late rows, not lost ones.

---

A late line on purpose
----------------------

`vdpctl late`: scene `full-4bpp-16-lim16`, with LOAD's handicap padding the build
of rows 100, 104, … 128 to 30,000 cycles, 34% past the budget, for 15 seconds with
snapshots streaming.

| Check | Result |
|---|---|
| late rows counted | 10,860 of 217,305 rows built: 12 a frame. No latch merged |
| rows late in snapshots | 1,308, in 109 snapshots: rows 100, 104, … 128, and often the row after each, whose build started late |
| each late row shows the most recently finished row | every one: 0 rows differ from the host's drawing of the row they show |
| status unaffected | the scene's program read `STAT1`, `STAT7`, `STAT8`–`STAT15` and `STAT0` at screen line 250 of frames 101–1006; all 906 frames match `vdp-scene --reads` |
| everything else | rows on time in the same snapshots all exact |

The status comparison works because a late row's build still runs to the end and
publishes (§18), and the program's reads come after the band's rows. A latch that
had to be merged would lose its row's status; none was.

---

Faults, the watchdog and safe mode
----------------------------------

Each kind raised from normal mode by `vdpctl fault`, then `vdpctl flash`
(`docs/results/phase-08/faults.txt`):

| FAULT | Record read from safe mode | Reset reason | Reflash |
|---|---|---|---|
| `core0` | HardFault on core 0, `CFSR` `$00010000` (UNDEFINSTR), PC `fault_raise` at `fault.c:223`, LR `dispatch` at `link.c:359` | fault | normal, no record |
| `core1` | HardFault on core 1, UNDEFINSTR, PC `fault_raise`, LR `apply_request` at `renderer.c:727` | fault | normal, no record |
| `hang` | watchdog: "core 1's renderer stalled", heartbeats core 0 626, core 1 17,027 | watchdog | normal, no record |
| `panic` | panic on core 0: "FAULT raised a panic on core 0" | fault | normal, no record |

In safe mode every row is dark red, the renderer is off, and INFO, REBOOT and FAULT
answer. `picotool load -x -f` reached the board through the SDK's reset interface
each time. No BOOT button was pressed in this phase.

---

On the host
-----------

| Check | Result |
|---|---|
| `cmake --workflow --preset host` | 21 of 21: the unit tests, `test_render`, `vdp_scene_check`, `vdp_replay` ×4, `oracle_pinned`, `spike_reference`, `node_addon_loads`, `replay` ×4 |
| `cmake --workflow --preset host-asan` | 16 of 16 under ASan and UBSan |
| `Video.test.ts` against the core | 342 of 342, none skipped |
| `tools/replay.mjs --classes`, `PICOVDP_SPLIT_CHECK=1` | fifteen checkpoints exact, all `static`, every event matched |
| `fuzz --scope frames`, seeds 1–4, 8 × 10⁶ | 109,807 frames, no divergence (27,454 + 27,512 + 27,463 + 27,378) |
| `fuzz --scope frames`, seed 5, 4 × 10⁶, split check | no divergence |
| `fuzz --scope tiles` seeds 1 and 2, `status`, `all`, `bus`, 10⁷ each | no divergence |
| `pico2`, `pro-debug`, `pro-release` | build, no warnings |

**`test_render`** holds the word renderer to Phase 7's pixel renderer, which the
goldens, Jest and the fuzzer had held to `Video.ts`. 1,500 random cards — every
geometry, legacy mode, depth, attribute source, flip, priority, scroll, sprite
size, depth and flag, tables wrapping VRAM — 17 lines each, built at ten splits
into two halves started from stale buffers, and with `vdp_build_line`: every row
and its collision status must match, and the halves' expansions must make the
whole's. 484,506 checks.

**`test_latch`** gained four tests of the latch in halves: a late catch-up takes
the card as it stood at its own latch; a full ring merges into its newest, counted;
status waits for `vdp_publish`, once; and 300,000 random operations with the render
side left behind for stretches, against a card that takes every latch at once —
every caught-up line's registers, VRAM, palette, line and sprite list identical.
104,826 checks.

**Mutants.** Eleven were planted in the word renderer before it was divided into
halves: ten were caught, by `test_render` (one only after it was given splits
that are not multiples of 4), and one, layer 1's judged level one higher, is
equivalent, since layer 1 only ever meets levels 0, 1 and 4. Seven more were
planted in the halves: six caught by `test_render` and the other unit tests; the
seventh changes only the split's cost. Three in the latch in halves were caught by
`test_latch`. Sources were restored after each, and every check above was run on
the final sources.

---

Done when
---------

| Check | Result |
|---|---|
| all fifteen checkpoints reproduce exactly on the Pico 2 via `vdpctl inject` (index frame, VRAM, registers) | ✅ all fifteen, and `STAT0`, every read and every `/INT` level |
| the worst-case scenes of Phase 1, drawn by the real core on both cores, run for ten minutes with zero late lines at `SPRLIMIT` 32, while `SNAPSHOT` streams continuously | ✅ all 36 for ten minutes between them, and the harshest ten minutes alone: 17.3 million rows, none late; 8,446 snapshots, every row exact |
| measured cycles are recorded against Phase 1 and §18 | ✅ [above](#the-renderer-a-word-at-a-time); SPEC.md §18 records them, in all three copies |
| a scene deliberately too heavy shows §18's late line — the previous line repeated — with its status unaffected, and the late lines are counted | ✅ 10,860 counted; every late row in 109 snapshots is its predecessor; 906 frames of status reads match the host |
| `vdpctl` triggers `FAULT`, reads the record from safe mode, and reflashes, with no hands on the board | ✅ HardFaults on both cores, a panic and a watchdog hang |
| the host suite passes, `host-asan` is clean, the firmware presets build | ✅ |

---

Differences from the plan
-------------------------

1. **The core's renderer was rewritten.** Phase 7's pixel loops took about three
   times the line on silicon. Layers and sprites now work in words, as Phase 1's
   spike did, and Phase 7's loops are kept only as `test_render`'s reference and
   for Text's 6-pixel cells.
2. **A row is built in two halves, one a core.** PLAN.md section 3 had core 0
   draw sprites into a sprite line for core 1 to merge after both layers. Measured,
   the halves leave more spare (12% against 9% on the harshest scene) and need no
   merge. The interface is `vdp_build_half`, `vdp_expand_half`, `vdp_copy_half`
   and `vdp_publish`; the split is on an 8-pixel boundary, not 32.
3. **The latch is split in the core.** `vdp_latch` in core 1's interrupt,
   `vdp_catch_up` in the renderer, records in a ring of 8, the journal a ring of
   1024: what lets a late line leave the next latch on time (§18). `vdp_line_start`
   is all three at once, so the host is unchanged.
4. **`OVF` is published with `COL`**, when core 1 finishes the row, rather than
   at the latch. Both are at the latch on the host.
5. **Core 0's half runs in an interrupt and the link in its thread.** Section 3 had
   it the other way round. A USB write blocks; a build beneath it would wait.
6. **Injection takes latches in core 1's thread,** in a trace replay's order, and
   applies each line's operations after the row it starts is built. Section 3 said
   at the latch; the emulator publishes a row's collisions at its latch, so a read
   later in the line must see them.
7. **The line start comes from the sync program,** a PIO interrupt at each screen
   line's back porch, not from the DMA interrupt, whose FIFO runs a line or two
   ahead. The RGB buffer is chosen there too, which is what makes a late line
   repeat its predecessor exactly.
8. **Extra commands.** SCENE, LOAD, SCENE LOG and PROFILE, and `vdpctl scene`,
   `load`, `scene-log`, `profile`, `scenes`, `late`: the scenes, the stand-in, the
   handicap and the stage timings the phase's checks needed. `vdp-scene` on the
   host is the reference for their snapshots.
9. **No `serialport`.** `vdpctl` opens the tty itself.
10. **SPEC.md changed, informatively, in all three copies:** §2 names the
    RP2354A, §18 describes the halves and records Phase 8's budget, and the
    Resolved Design Question on `SPRLIMIT` quotes both measurements. Nothing
    normative moved and no golden changed. The HTML rendering, published from
    outside the repo until then, was updated after the phase's commit and now
    lives at `docs/SPEC.html`.
11. **Emulator:** `docs/VDP-SPEC.md` only.

---

For later phases
----------------

- **Phase 10.** `pro-debug` builds and has not run: the PRO's VGA pins are the
  Pico 2's, and the image fits its 2 MB with room. Repeat `inject all`, `scenes`,
  `late` and the faults there.
- **Phase 11.** The bus handler lands on core 1 beside the latch, and the stand-in
  says the margin at `SPRLIMIT` 32 will not hold for the heaviest scenes. What is
  left to take out of a row: evaluation (1,817 cycles, against the spike's
  1,258), the split's choice (about 510), the level contest of a judged
  layer, the two halves' clearing. The renderer's hottest code is now duplicated
  per depth and source; measure before adding more copies.
- **Phase 13.** `OVF` and `COL` now reach status when core 1 finishes the row.
  Measure their lag with a real CPU, and update §18's budget from the PRO under the
  Nano's traffic.
- **Open.** Two PROFILE answers repeated the previous scene's figures and did not
  recur.

---

Reproducing
-----------

```sh
cmake --workflow --preset host && cmake --workflow --preset host-asan
cmake --preset pico2 && cmake --build --preset pico2
node tools/vdpctl.mjs flash
node tools/vdpctl.mjs inject all
node tools/vdpctl.mjs scenes --seconds 16.7 --stream
node tools/vdpctl.mjs scenes --minutes 10 --stream --only full-4bpp-32-det
node tools/vdpctl.mjs scenes --seconds 5 --bus 500000
node tools/vdpctl.mjs late --seconds 15
for f in core0 core1 hang panic; do node tools/vdpctl.mjs fault $f; node tools/vdpctl.mjs flash; done
node tools/vdpctl.mjs profile --row 101
```

Raw results are in `docs/results/phase-08/`.
