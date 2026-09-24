Phase 13 — Raster timing, interrupts and load
=============================================

**Status:** done on 2026-09-23. Every criterion in PLAN.md's Phase 13 is met,
on the PICO9918 PRO running this repository's `pro-debug` build and then its
`pro-release` build. The first full load runs were not green: the card counted
a stale read the Nano never saw, about once an hour, and the host's port reader
stopped now and then. The first was a glitch on the harness's `/CSR` that the
read program took for a read, and is fixed on the card; the second is the
host's, and the load run now rides it out ([Found on the way](#found-on-the-way)).

The last of Part B: what only the raster and a real CPU's pace can show. §3's
latch on the pins, the interrupts' timing against the raster, how fresh a
status read is, the card under everything at once for an hour, and — made a goal
of this phase by Phase 11 — a 2 MHz 6502's accesses, 2 µs apart, right every
time.

**The headline.**

- **A 2 MHz 6502's accesses work.** Under an hour of the load run, 33.6 million
  trials of accesses exactly 2 µs apart — reads back to back, a read straight
  after its address command, writes — on each pair in turn: **none wrong**, and
  the card's stale-data count 0. Phase 11's conformance stream at 2 µs: 10⁷
  accesses, none wrong. Phase 11 had found such reads stale once in 170.
- **The load run, an hour:** the worst reset-state scene with `FONT` loads for
  both layers every frame, the Nano's 2 MHz bursts, a scanline interrupt every
  eight lines, snapshots streaming. **No late line, no FIFO overrun,** all
  23,804 snapshots right and all 60 captures stable. The worst row is 3.6%
  inside its line.
- **§3's latch holds on the pins:** 11,800 trials, the change always from line
  N + 2, N through picture, border and blanking.
- **The interrupts are where §3 puts them:** vertical blank every 262.5 lines,
  59.94 Hz, steady to 0.025 µs; every `IRQLINE` in every geometry within 0.1 µs
  of its line; `/INT` 1.35 µs after its line begins on average, 3.24 µs at
  most, with the odd VGA line where §3 says.
- **Status is fresh to the line** (Still Open 4, closed): every `STAT2` read
  inside a line showed that line. `STAT3` b1 is driven for the first time.
- **The release build matches** through the pins: Phase 11's scripts and
  streams at each profile and at 2 µs, and Phase 12's replays.
- **At `SPRLIMIT` 32, Full mode's sprite-heavy lines are late under 2 MHz
  bursts;** at the reset 16 all fit, with 6–19% spare. §18 now says so.

Contents: [What was built](#what-was-built) ·
[2 MHz](#2-mhz) ·
[The latch](#the-latch) ·
[Interrupt timing](#interrupt-timing) ·
[Status freshness](#status-freshness) ·
[The load run](#the-load-run) ·
[The real bus's cost](#the-real-buss-cost) ·
[Release parity](#release-parity) ·
[Found on the way](#found-on-the-way) ·
[Done when](#done-when) · [Differences from the plan](#differences-from-the-plan) ·
[For later phases](#for-later-phases) · [Reproducing](#reproducing)

---

What was built
--------------

**The core.**

- **The latch in two parts.** `vdp_latch_take` is the latch's one moment —
  the line's events (§14) and where the journals end — and `vdp_latch_record`
  builds the record from what was taken, which the bus may interrupt. On the
  host, `vdp_latch` and `vdp_line_start` are both at once, as before.
- **Registers are journaled** as VRAM writes are: each write leaves its byte in
  a ring of 256, and a latch notes where the ring ends. Copying the whole
  register file into every record was most of what the latch used to spend. A
  reset, or a ring that filled, has the next latch copy the file whole.
- **A `FONT` load rides in the latch record.** The render side takes it at its
  place among the writes. The bus copy's 2 KB go in afterwards, in 64-byte
  chunks, each tracked by a bit: the renderer thread copies them a chunk at a
  time with the bus masked for each, and an access that reaches a chunk not
  yet copied copies that chunk first. So no access after the latch sees the
  load unfinished (§7), and none waits for more than one chunk.
- **The geometry is kept resolved** on the bus side, as the mode registers are
  written, instead of being looked up at every latch and every restage.
- **`vdp_set_hblank`** says whether a port's answer moved: only a port whose
  `STATSEL` names `STAT3` reads the bit.
- Hot bus-side functions carry `VDP_BUS`, which puts them in core 1's own
  scratch bank on the RP2350.

Unit tests: the chunked copy (`copied_after_the_latch_a_step_at_a_time`,
`an_access_that_reaches_it_copies_its_chunk`, `an_access_elsewhere_does_not`,
`a_reset_after_the_latch_finishes_it`), the register journal's overflow and
merge (`the_register_journal_overflows_to_a_whole_copy`), and
`hblank_moves_an_answer_only_where_stat3_is_read`. The random late-against-
prompt comparison in `test_latch` covers the rest. All 364 Jest tests pass
against the core, and the fuzzer ran 10⁷ operations in each of the bus, status,
tiles and frames scopes with no divergence.

**The firmware.**

- **The bus is above everything on core 1** (PLAN.md section 3). The latch
  moved to priority `$40`, horizontal blanking to `$20`, and the bus and RST
  stayed at `$00`. What holds the bus off is short: the latch's moment, a
  chunk of a `FONT` copy, a row's publication and the debug build's own card
  accesses each mask core 1's interrupts for under 300 cycles.
- **Restaging happens in the bus interrupt only.** Anything else that moves what
  a port reads asks for a restage with a flag and the interrupt's pending bit.
  The interrupt takes one access a run — the PIO holds it asserted while
  another waits — and an access that moves only a prefetch restages the two
  data bytes alone.
- **The read program checks `/CSR` twice:** 23 ns after it falls, as before,
  and again about 120 ns in. A strobe that has gone high by then is a glitch,
  and is not reported to core 1 ([Found on the way](#found-on-the-way)).
- **`STAT3` b1** is driven from the VGA timing for the first time
  ([Found on the way](#found-on-the-way)): the sync program marks each VGA
  line's front porch, and a third state machine times the 320-tick blank and
  raises an interrupt at either end, to core 1 only.
- **The shared timer.** SIO's RISC-V machine timer, set to count at the system
  clock, lets one core's moment be set against the other's: the raster probes
  in STATS (docs/DEBUGLINK.md) — what held the bus off, line start to latch,
  line start to status staged, `OVF` and `COL` publication, and the frame.
- LOAD can move the scene's program to port B.

**The harness** (firmware 3, protocol 3): PACED, INT_RUN and TRAFFIC
(docs/BENCH.md section 5). **The host:** `vdpctl two-mhz`, `latch`,
`irq-raster`, `freshness`, `load-run`, `scenes --nano-traffic`, and
`conformance --paced`; `conformance` and `replay` with `--no-link`, for a
release build.

---

2 MHz
-----

Phase 11 found data reads 2 µs apart stale about once in 170: a read landing in
core 1's latch interrupt waited for all of it before its restage. PLAN.md gave
this phase three candidates — let the bus preempt the latch with the latch's
writes made safe, shorten the latch, or have the read program serve the next
byte itself. The third cannot help a read straight after an address command,
whose byte only the write names. So the first two, together.

**The budget.** The Nano's PACED runs put every access exactly 2 µs after the
last; a read's `/CSR` rises 1.625 µs before the next access's falls, 572 cycles
at 352 MHz, and the restage has to be in the staging FIFO a few cycles before
that. A 2 MHz 6502's strobe is half a cycle, so it leaves 1.75 µs, 616 cycles.
The worst an access waits is the longest masked section on core 1, plus the
bus interrupt's own path to the staging FIFO.

**How it got there,** each step measured through the pins with every access
2 µs apart — reads back to back, a read straight after its address command,
writes back to back — under Full mode's heaviest reset-state scene
(`full-4bpp-32-det-lim16`: two 4bpp layers, 16 magnified sprites a line,
detailed collision) with `FONT` loads for both layers every frame:

| Firmware | Reads | Wrong | What held the bus off, worst | Bus interrupt |
|---|--:|--:|---|---|
| Phase 12's | 65,500 | 230 (0.35%) | the whole latch, same priority | — |
| bus above the latch; restage by request; `FONT` copy in steps | 131,000 | 6 | latch 505, publication 397 | ~250 |
| + geometry kept, fonts in the record, publication merged | 873,344 | 0 | latch 322 | ~250; 1,472 late rows |
| + data-only restage | 873,344 | 0 | latch 322 | ~170 |
| + the latch's moment split from its record, 64-byte chunks copied by the thread, the read program's second glitch check: the load runs | 19,235,057 | 0 | latch 244, a chunk 163, publication 134 | 152 mean, 320 at most |

The card's own stale-data count agreed with the Nano's wrong reads every time,
but for the reads the Nano never made ([Found on the way](#found-on-the-way)).
The worst of what holds the bus off and the worst bus interrupt, 244 and 320
cycles, sum to less than the 572 even if they fall together.

Two things in that table were not about staleness. At the third step, 2 µs
traffic made **1,472 rows late**: the real bus interrupt then cost about 250
cycles an access, a third of core 1 at that pace. The data-only restage brought
it to about 170, and taking one access a run and reusing the FIFO status to 138
— 152 with the stale records the debug build now keeps ([The real bus's
cost](#the-real-buss-cost)). And the chunked copy came from a
run of four stale reads in one burst: the trial window ended where layer 0's
font landed, a read's prefetch touched the unfinished copy, and the guard then
finished all 4 KB inside the bus interrupt — about 10 µs with the bus held.
Copying only the chunk an access needs makes that wait one chunk.

**The criteria.** Under the load run ([below](#the-load-run)), thirty minutes
with the Nano on each pair:

| 2 µs apart | Pair B | Pair A | Wrong |
|---|--:|--:|--:|
| Data reads back to back | 7,086,529 | 7,156,272 | 0 |
| A data read straight after its address command | 2,483,905 | 2,508,351 | 0 |
| Data writes back to back, each read back | 7,159,586 | 7,230,048 | 0 |

Every one is at least the 10⁶ the plan asks for, on each pair, and the card's
stale-data count was 0 in both runs. Phase 11's conformance stream, played with
every access exactly 2 µs after the last (`conformance --paced`): 10,000,595
accesses, 1,475,718 reads compared, no batch mismatching and all 17 whole-card
checks equal, 0 stale data reads.

---

The latch
---------

§3: a register write made while line N is on screen shows from line N + 2.
The Nano's scanline handler, at `IRQLINE` N, changes `COLOR` and `L0SCRX`, and
sets `IRQLINE` to N + 131; there it puts them back. Every snapshot shows where
each change first appears, row by row, against where §3 puts it.

In Compact mode, whose borders put picture, border and blanking all within
reach of `IRQLINE`: 59 values, every sixth from 0, and two either side of
each edge between picture, border and vertical blanking, of the odd VGA line,
of the frame's end and of 255 — 100 frames each.

**11,800 trials over 5,900 frames: no row wrong, none late.** Each change
showed from exactly N + 2, whether N + 2 was in the picture, a border or
vertical blanking, and whether the handler's write landed early in its line or
late.

The negative control holds each handler 69 µs, past the next line's start, so
its writes land in line N + 1 and must show from N + 3: at 100, 200 and 250,
every trial whose rows are on screen failed (50 rows wrong of 60 trials; the
ten that pass are line 257's, which no snapshot shows). The test can fail.

---

Interrupt timing
----------------

Every interval here is between `/INT` edges, timed by the Nano's capture
unit to 62.5 ns.

**Vertical blank:** 585 periods, 16.68989 ms by the Nano's clock, with a
standard deviation of 0.025 µs and all of them within 16.68981–16.68994 ms. By
the card's own clock the frame is 5,872,650 cycles on average,
5,872,512–5,872,786 under the load run: 262.5 lines of 22,372, 16.68366 ms,
59.94 Hz. The two differ by the Nano's resonator ([Found on the
way](#found-on-the-way)); everything below is in lines of the frame the Nano
measured.

**Scanline compare,** every `IRQLINE` from 0 to 255 in each geometry, each
timed against vertical blank's `/INT` over several frames:

| Geometry | Fired | Worst against §3's line | Largest spread | Steps other than one line |
|---|--:|--:|--:|---|
| Text | 256 of 256 | 0.086 µs | 0.125 µs | 237 → 238: 1.500 lines |
| Compact | 256 of 256 | 0.085 µs | 0.125 µs | 237 → 238: 1.501 lines |
| Graphics | 256 of 256 | 0.088 µs | 0.125 µs | none |
| Full | 256 of 256 | 0.092 µs | 0.125 µs | none |

The spread is two ticks of the Nano's capture. The step of a line and a half
is the odd VGA line (§3): in Text and Compact it falls between display lines
237 and 238, where `IRQLINE` reaches; in Graphics and Full, between 261 and 0,
where it does not.

**Where `/INT` falls in its line** is the card's to measure: core 0 notes the
shared timer as each line begins, and the bus interrupt notes it as it
restages for that line's latch. Over 66 million lines of the two load runs:
1.35 µs after the line began on average, 3.24 µs at most. A line begins at the
back porch before its first VGA line, so `/INT` falls near the line's first
visible pixel.

**Overflow and collision** reach `STAT0`, `STAT1` and `/INT` once the row that
found them is published: 41.2 µs after the start of the line before on
average, 59.7 µs at most — before the row is shown, as §3 has it.

---

Status freshness
----------------

§6 and Still Open item 4: how far behind the raster a status read can be.
The Nano's scanline handler reads `STAT2` (the display line) at a chosen delay
after `/INT` falls — from 7.8 µs to 81 µs, 10,000 edges — and each read is
compared with the line the handler was for. An edge the Nano served late, with
a host exchange in the way, is left out: 357 of them at the first delay and 36
at each other.

**Every read inside the line showed the handler's line;** every read past the
next line's start showed the next. At 56.2–56.7 µs after `/INT` all 972 reads
were the line itself, at 65.2–65.7 µs all 972 were the next. So the byte a
program reads is the line it is in, give or take the 1.35 µs average (3.24 µs
at most) the restage takes after each line start.

**`STAT3` b1,** sampled at 542 instants across the line: set from 26.25 to
32.94 µs after `/INT` and from 58.00 to 64.31 µs, 6.69 and 6.31 µs — the two
VGA lines' horizontal blanking, 6.36 µs each, a few tenths of a microsecond
late. Only seven instants, at the edges, read both ways.

---

The load run
------------

Everything at once, for thirty minutes with the harness on each pair:

- **The worst reset-state scene,** `full-4bpp-32-det-lim16` — Full mode, two
  4bpp layers, 16 magnified sprites on every line, detailed collision — its
  program on the other pair, with **`FONT` loads for both layers every frame**.
- **The Nano's fastest traffic** on its pair: the 2 MHz trials above, in
  bursts of 100 accesses 2 µs apart, one after another.
- **A scanline interrupt every eight lines,** which the Nano serves between
  bursts: each handler reads `STAT1` and moves `IRQLINE` on by eight.
- **Snapshots streaming** over USB, each whole frame checked against the host's
  `vdp-scene`, and **a capture every minute**.

| | Pair B | Pair A |
|---|--:|--:|
| Bursts of 2 µs accesses | 219,170 | 221,327 |
| Scanline handlers served | 3,353,735 | 3,350,822 |
| Snapshots, every row checked | 11,801 | 12,003 |
| Rows wrong, rows late | 0, 0 | 0, 0 |
| Late rows, merged latches, FIFO overruns | 0, 0, 0 | 0, 0, 0 |
| Stale data reads | 0 | 0 |
| Captures, each picking out one frame | 30 of 30 | 30 of 30 |
| Worst row, latch to buffer, of 22,372 | 21,562 (3.6% spare) | 21,550 (3.7%) |

**Zero late lines, zero FIFO overruns, and a stable capture,** as the plan
asks. Each capture was judged against every frame the card showed between the
snapshots either side of it: the best was 51.1–51.8 levels out, the rest at
least 80.5, and every one at the same offset, 3,−1. The scene scrolls every
frame and is detail at every pixel, so no capture can come closer than that;
what matters is that it picks out one frame, clearly, every time
([Differences from the plan](#differences-from-the-plan), 6).

The Nano served about 97% of the handlers the card raised: an edge that falls
while it waits on the host for its next burst waits with it, up to a
millisecond, and the card's next compare comes regardless.

On pair B the host's port reader stopped once, 131 s in, for 30 seconds
([Found on the way](#found-on-the-way)); the run reopened it and went on, and
the card's counts are for the whole thirty minutes.

---

The real bus's cost
-------------------

What a real CPU's accesses cost the card, from the load runs' probes
(cycles at 352 MHz; the debug build, which keeps the counts):

| | Mean | Most |
|---|--:|--:|
| The bus interrupt, per access | 152 | 320 |
| Core 1 holding the bus off: the latch's moment | | 244 |
| — a `FONT` copy's 64-byte chunk | | 163 |
| — a row's publication | | 134 |
| — the thread's own card accesses | | 157 |
| Line start to its latch (the doorbell) | 80 | 454 |
| Line start to status staged, `/INT` driven | 475 (1.35 µs) | 1,139 (3.24 µs) |
| Latch interrupt, whole | | 1,249 |

At 2 µs, 704 cycles, an access costs core 1 about a fifth of its time while a
burst lasts. The spike's stand-in cost about 100 an access; the real interrupt
began at 250 and was brought to 152 ([2 MHz](#2-mhz)), the stale records
included, which the release build does not keep.

That cost comes out of the renderer's margin. Full mode's worst cases for 60
seconds each, under the Nano's 2 MHz bursts on port B (`scenes
--nano-traffic`), latch to buffer against the 22,372-cycle line:

| Full mode, 4bpp, two layers, 2 MHz bursts | 32 sprites on the line | Spare | 16 sprites on the line | Spare |
|---|--:|--:|--:|--:|
| 16 × 16 sprites | 24,354 | late | 18,076 | 19% |
| 16 × 16, detailed collision | 30,121 | late | 19,350 | 14% |
| Magnified | 28,612 | late | 19,471 | 13% |
| Magnified, detailed collision | 33,447 | late | 20,935 | 6% |

No access was wrong in any of them. At the reset `SPRLIMIT` of 16 every case
fits; with `FONT` loads for both layers every frame and snapshots streaming as
well, the last of them is the load run's 3.6%. At 32, all four make late rows
under this traffic — the stand-in had the plain 16 × 16 case fitting, with 6%
— and §18 now says so. A late row is specified and counted (§18), and none of
these is a reset state.

---

Release parity
--------------

`pro-release` has no USB, no counts and no link, so it was checked from the
outside only: flashed from the debug build with `picotool load -x` (no button),
then Phases 11 and 12 again through the pins, with VRAM read back through the
bus and the capture card standing in for `SNAPSHOT` and `VRAM` (`--no-link`).

| Through the bus, release build | Result |
|---|---|
| Phase 11's directed scripts, at each profile | all passed |
| Back-to-back data reads 4 µs, 2 µs and flat out (1.16 µs) apart | 12,000 each, **all right** |
| Phase 11's stream, 10⁷ accesses at each of the three profiles | 1,475,718 reads compared each, **0 wrong**; all 16 whole-card checks equal |
| The same stream with every access 2 µs apart (`--paced`) | **0 wrong**; all 17 whole-card checks equal |
| Phase 12's replays, every fixture at each profile, cold-started through the bus | **54 of 54** checkpoints exact — each frame from the capture card, VRAM read back through the bus — and every compared read right |

A whole-card check without the link is all 64 KB of VRAM read back through the
pins and compared against `Video.ts`, as the stream's own reads are. The
registers are write-only, so without the link they are checked only by what
they do to the reads.

The board is left running `pro-release`, for Phase 14. Going back to
`pro-debug` needs the BOOT button and a replug.

---

Found on the way
----------------

- **`STAT3` b1 was never driven on the RP2350.** PLAN.md section 3 has the
  platform supply it from the VGA timing, and the core has taken it through
  `vdp_set_hblank` since Phase 4, but no firmware called it: on the PRO b1 read
  0 always. Nothing compared it — §6 calls it advisory and PLAN.md section 4
  leaves it out of the comparisons on silicon — so nothing noticed. It is driven
  now, and measured below.
- **A request could reach core 1 half written.** The debug link hands core 1 a
  request as a plain struct and a volatile sequence number, and the compiler
  was free to store the number first; the disassembly showed it did. Phase 8 to
  12 never caught it. With core 1 busier, it did: a VRAM request whose pointer
  core 1 read before core 0 wrote it, and a HardFault in `memcpy` writing from
  address 0 up to the end of the ROM. The fault record named it at once. Barriers
  now order it, and the state-at-line-239 seqlock, the line-start events and the
  finished-row flag, which have the same shape.
- **A read the CPU never made.** Three load runs each counted one or two stale
  data reads that the Nano, checking every read, never saw. The record the
  debug build now keeps of each explained it: the read was taken about 150
  cycles after the one before it, where the Nano's are 704 apart, and was served
  the same byte. The Nano turns its data bus around, from a read to the write
  that follows it, with eight outputs switching at once, and once in about 2.5
  million turnarounds that pulled its `/CSR` low for longer than the read
  program's 23 ns check. The program answered it, told core 1, and the port's
  pointer moved on one more than the CPU's. Only an address command follows a
  read that way in the trials, and it sets the pointer again, so no read the
  Nano made was wrong. The read program now checks `/CSR` a second time, about
  120 ns after it fell, and drops a strobe that has gone high again without
  telling core 1. A 6502 at 2 MHz holds it low for most of its 250 ns. The two
  instructions it needed came from folding the port table (bus.pio): port 3
  now drives the bus as late as port 2 always has, 17 cycles, 48 ns, and port 1
  a cycle sooner.
- **The host's port reader can stop.** Every ten to thirty minutes of a load
  run, the process streaming snapshots stopped receiving: nothing for 30
  seconds, while another process opening the same port meanwhile was answered
  at once. The card was not waiting on anything; the host's tty reader was.
  The load run now reopens the port, once for every request then in flight,
  and records each time; the card's counts run on regardless.
- **The Nano's clock is not the card's.** It measures the frame at 16.6899 ms;
  the card, on its own crystal, at 5,872,649 cycles, 16.68366 ms — exactly
  262.5 lines of 22,372. The Nano's resonator runs 0.038% fast. Everything below
  that the Nano times is in lines of the frame it measured.

---

Done when
---------

PLAN.md's Phase 13:

| Check | Result |
|---|---|
| the latch holds on all trials: 10⁴, N swept through picture, border and blanking, the change from N + 2 | ✅ **11,800 of 11,800**; the late control fails every trial it can see |
| 2 MHz: back-to-back data reads 2 µs apart, 10⁶ each pair, under the load run | ✅ 7,086,529 on B and 7,156,272 on A, **0 wrong** |
| a data read 2 µs after its address command, 10⁶ each pair | ✅ 2,483,905 and 2,508,351, **0 wrong** |
| writes 2 µs apart land, 10⁶ | ✅ 7,159,586 and 7,230,048, each read back, **0 wrong** |
| Phase 11's conformance stream at 2 µs | ✅ 10,000,595 accesses, **0 wrong**, in the debug build and the release build |
| the card's stale-data count stays 0 | ✅ **0** in both load runs, the paced stream and every scene under traffic |
| the load run: zero late lines, zero FIFO overruns, a stable capture | ✅ **0, 0**, 60 of 60 captures |
| the release build matches | ✅ Phase 11 at each profile and at 2 µs, and Phase 12's replays, through the pins |
| the measurements in this file and in SPEC.md §3, §14, §18 and Still Open — all three copies | ✅ also §4 and §6; the emulator's copy is identical, and the HTML is republished |
| the host suite passes, the firmware presets build | ✅ 28 tests, 21 under ASan and UBSan, all 364 Jest tests against the core, the fuzzer's 10⁷ operations in each scope; `pico2`, `pro-debug` and `pro-release` build |

---

Differences from the plan
-------------------------

1. **The 2 MHz fix is two of the three candidates at once,** and more: the bus
   above the latch, with the latch's own moment made short — registers
   journaled, the record built after the moment, `FONT` copies in chunks — and
   the bus interrupt itself trimmed. The third candidate, a read program that
   serves the next byte on its own, cannot help a read straight after an address
   command, which the criteria include.
2. **`STAT3` b1 was built here,** not only measured: no earlier phase had driven
   it ([Found on the way](#found-on-the-way)).
3. **No frame-sync tap.** PLAN.md left it to this phase; every interval is
   measured between `/INT` edges, and where `/INT` falls against the raster the
   card measures itself on a timer both cores share (docs/BENCH.md section 4).
4. **The load run is an hour, not thirty minutes:** thirty with the harness on
   each pair, since one CPU's accesses cannot share a pair with another's (§4).
   The scene's program takes the other pair (LOAD's new flag).
5. **"The Nano's fastest traffic"** is bursts of 100 accesses exactly 2 µs apart,
   a host round trip between them — the fastest a 6502 at 2 MHz makes and
   faster than any it sustains — not the harness's flat-out 1.1 µs, which no
   AC6502 can produce.
6. **"A stable capture"** is judged by what a capture of that scene can show: it
   scrolls every frame and is detail at every pixel, so Phase 10's tolerance,
   set on the oracle's pictures, cannot apply. Every capture must pick out one
   of the frames the card showed around it, clearly better than the rest, where
   the dongle puts every picture ([The load run](#the-load-run)).
7. **Release parity** could not use the debug link at all, so the replay's cold
   start, frames and VRAM, and conformance's whole-card checks, are done through
   the bus and the capture card instead (`--no-link`).
8. **The latch test counts frames, two trials each,** not writes: every frame
   the handler changes the state at N and puts it back at M, so one snapshot
   checks both transitions.
9. **"The worst-case scene" is the worst at the reset `SPRLIMIT` of 16.** At 32,
   every Full-mode sprite case makes late lines under 2 MHz bursts alone
   ([The real bus's cost](#the-real-buss-cost)), so a load run there would
   show late lines by construction. §18 specifies them and now says where they
   happen; the load run's zero is for the card as a program finds it.

---

For later phases
----------------

- **Phase 14** takes `pro-release` into the AC6502. The board is left running it
  ([Release parity](#release-parity)); `pro-debug` again needs the BOOT button
  and a replug.
- **The budget under a real CPU is tighter than the stand-in said.** An access
  costs core 1 about 150 cycles in the debug build, and a burst of them at 2 µs
  takes the heaviest reset-state row to within 4% of its line. §18 has the
  table. At `SPRLIMIT` 32 the same traffic makes Full mode's sprite-heavy lines
  late. If a program ever needs them there, the remedies are the renderer's
  (PLAN.md risk 1).
- **The oracle has nothing that reads VRAM back** (Phase 12). The 2 MHz trials
  and the load run read tens of millions of bytes back through the pins, but
  none of them is a golden.

---

Reproducing
-----------

```sh
cmake --workflow --preset host
cmake --preset pro-debug && cmake --build --preset pro-debug
export PICOVDP_PRESET=pro-debug
node tools/vdpctl.mjs flash
(cd bench/nano && pio run -t upload)    # harness firmware 3: PACED, INT_RUN, TRAFFIC

node tools/vdpctl.mjs two-mhz
node tools/vdpctl.mjs load-run --minutes 30 --pair B --out load-run-B.json
node tools/vdpctl.mjs load-run --minutes 30 --pair A --out load-run-A.json
node tools/vdpctl.mjs reset --power-on
node tools/vdpctl.mjs latch --out latch.json
node tools/vdpctl.mjs latch --lines 100,200,250 --frames 10 --late   # the negative control: must fail
node tools/vdpctl.mjs freshness --edges 10000 --out freshness.json
node tools/vdpctl.mjs irq-raster --out irq-raster.json
node tools/vdpctl.mjs conformance --paced --ops 10000000 --out conformance-paced.json
node tools/vdpctl.mjs scenes --nano-traffic --seconds 60 --only full-4bpp-16,full-4bpp-16-det,full-4bpp-32,full-4bpp-32-det,full-4bpp-16-lim16,full-4bpp-16-det-lim16,full-4bpp-32-lim16,full-4bpp-32-det-lim16

# Release parity: flashed from the debug build, no button; back needs BOOT.
cmake --preset pro-release && cmake --build --preset pro-release
picotool load -x -f build/pro-release/firmware/picovdp.uf2
node tools/vdpctl.mjs conformance --no-link --timing all --ops 10000000
node tools/vdpctl.mjs conformance --no-link --paced --ops 10000000
node tools/vdpctl.mjs replay all --timing all --no-link
```

Raw results are in `docs/results/phase-13/`.
