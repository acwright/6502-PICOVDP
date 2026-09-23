Phase 11 — The bus
==================

**Status:** done on 2026-09-23. Every criterion in PLAN.md's Phase 11 is met.
One measurement goes past what the plan asked, and falls short there: reads
back to back at a **2 MHz** 6502's spacing are occasionally stale —
[Back to back](#back-to-back).

The part of SPEC.md that had never run on hardware: four ports through real
pins, driven by the Nano, on the PICO9918 PRO with this repository's firmware.

**The headline.**

- **All four ports work through the pins.** 10⁷ accesses at each of the three profiles — 30,001,785
  in all, 5,178,747 of them reads and 4,427,154 of those compared with
  `Video.ts` — with no read wrong, and all 48 whole-card comparisons over the
  debug link (VRAM, the 128 registers, both ports) exact. The card counted
  exactly the writes and reads the Nano made.
- **`MDE1` is proven.** Phase 9 could not test the soldered pin — the stock
  firmware ignores MODE1 — and every access on ports 2 and 3 in this phase went
  through it.
- **Reads 4 µs apart are always right.** 12,000 data reads back to back on one
  port at exactly a 1 MHz 6502's spacing, alternating the pairs, each returning
  the prefetch the read before it fetched: none wrong.
- **At 2 µs they are not.** About 1 read in 170 at a 2 MHz 6502's spacing
  returns the byte before it, because the restage behind it is still waiting on
  core 1's latch interrupt. The card counts every one of them itself. The plan
  asks for 4 µs; what 2 µs needs is Phase 13's to weigh
  ([below](#back-to-back)).
- **RST performs §15.** 50 pulses out of 50, each from a scrambled card with a
  `FONT` load pending, leave the state §15 names: every register, both ports,
  all 64 KB of VRAM as `Video.ts` has it after its own reset — the built-in font
  at `$0800`–`$0FFF`, the palette back in its window, the pending load
  cancelled — status clear microseconds after the pulse, and `/INT` released.
- **The first read program was 20 ns too slow for the bench,** and read one
  bit wrong within the first 72,000 accesses at the harness's narrowest strobe. The program that
  shipped selects its byte through a jump table while `/CSR` is still being
  debounced, and has not missed since ([below](#the-read-program)).
- **No FIFO ever overran,** and no access arrived closer to another than the
  bus interrupt could tell them apart.

Contents: [What was built](#what-was-built) ·
[The read program](#the-read-program) ·
[Four ports, 10⁷ accesses at each profile](#four-ports-10-accesses-at-each-profile) ·
[Back to back](#back-to-back) ·
[Stale status, and what a stale read acknowledges](#stale-status-and-what-a-stale-read-acknowledges) ·
[RST](#rst) · [Strobe and hold margins](#strobe-and-hold-margins) ·
[The harness had to get faster](#the-harness-had-to-get-faster) ·
[Done when](#done-when) · [Differences from the plan](#differences-from-the-plan) ·
[For later phases](#for-later-phases) · [Reproducing](#reproducing)

---

What was built
--------------

**The core** gained two functions, for a read that the pins answer before the
firmware hears of it (§2, §6):

- `vdp_staged` — what each port would read now, a byte a port in A1:A0's order:
  data A, status A, data B, status B. It changes nothing.
- `vdp_read_served` — a read the pins have already answered with a given byte.
  Served what `vdp_read` would return, it *is* `vdp_read`. Served a byte the
  card has since moved past, a data read moves its port on as usual, and a
  status read acknowledges only what it showed: a flag the CPU never saw is not
  cleared ([below](#stale-status-and-what-a-stale-read-acknowledges)).

`tests/unit/test_served.c` holds both: the staged word is each port's read, and
over 200,000 random operations with line starts, interrupts and reads on both
pairs a card read through `vdp_read_served` is byte for byte the card read
through `vdp_read`. The Jest suite, all 364 tests, and the fuzzer are unchanged
and green, since `vdp_read` itself did not move.

**The firmware** (`firmware/bus.pio`, `firmware/bus.c`), on pio1 and core 1:

- `tmsWrite` is pico9918's, unchanged, with its idle TX FIFO joined to RX for
  eight words of slack. Its handler takes the port from bits 31:30 and the byte
  from 7:0.
- `tmsRead` is new ([below](#the-read-program)): it answers from one staged
  word holding all four ports' bytes, and pushes back which port it answered and
  with what.
- One interrupt takes both FIFOs, at the latch's priority on core 1, as PLAN.md
  section 3 has it: writes through `vdp_write`, reads through `vdp_read_served`,
  then a restage. A read and a write both waiting would mean they came closer
  together than the interrupt answers and their order is lost; they are taken
  write first and counted.
- **Restaging** is `bus_sync`: stage the word, and drive `/INT` on GPIO 22 to
  `vdp_int_asserted`. It runs after every access, at every latch, after every
  row's publication, after a reset and after anything the debug link does
  through the card.
- **RST** on GPIO 23 performs §15 on its falling edge, after taking any
  accesses already waiting.
- **Statistics** (debug builds): accesses taken, reads served stale — data and
  status apart — coincident accesses, FIFO overruns for each program, staging
  waits, RST edges, the interrupt's longest run and the `/INT` level. They are
  appended to `STATS` (docs/DEBUGLINK.md).
- The bus starts only on the PRO: a Pico 2 has nothing on those pins, and its
  GPIO 23 sets its regulator's mode.

**The harness** gained `SCRIPT` and `READ_RUN` (docs/BENCH.md section 5), and
**the host** `tools/lib/conformance.mjs` behind `vdpctl conformance` and
`vdpctl reset-pin`, with the directed scripts in `tests/bench/*.bus`.

---

The read program
----------------

The first version followed pico9918's shape: wait for `/CSR`, debounce it for
eight cycles, sample MODE and MODE1, then shift the staged word right a byte at
a time until it reached the port's, and drive. That is 24 cycles from the edge
to the bus for port 0 and 33 for port 3 — 68 to 94 ns at 352 MHz.

At `6502-1mhz`, whose reads the Nano samples 562 ns after `/CSR` falls, it was
perfect. At `6502-2mhz` and `fastest`, sampled at the harness's floor of
187.5 ns, it failed: after 72,000 accesses at one and 28,000 at the other, a
read of `STAT4` came back `$BC` for `$AC` — one bit late. The card had served
the right byte (it counted no stale read, and every whole-card comparison
agreed), so the bus was being sampled before it had settled: 187.5 ns less the
AVR's input synchroniser, less the PRO's 5 V buffers, left the program about
20 ns short. The stock firmware, with one byte fewer to choose between, had
passed the same test in Phase 9.

The version that shipped (`firmware/bus.pio`) does its choosing while it
debounces:

- it sits at offset 0 of pio1, so `mov pc, isr` on the two sampled pins jumps
  straight to the port's slot: `jmp` for ports 0 to 2, `out null, 24` for port 3;
- the byte is shifted into place and latched onto CD0–7 with the pins still
  inputs, during the debounce;
- after the debounce's check, one instruction — `mov pindirs, ~null` — drives
  the bus.

`/CSR` to CD0–7 driven is 15 cycles for ports 0 and 3 and 17 for 1 and 2, 43 to
48 ns, and the debounce is still 8 cycles. After the change, the 20,001,190
accesses of the two profiles sampled at that floor have not produced a wrong
bit.

---

Four ports, 10⁷ accesses at each profile
-----------------------------------------

`vdpctl conformance --timing all --ops 10000000`
(`docs/results/phase-11/conformance.txt` and `.json`): at each profile, the six
directed scripts in `tests/bench/`, then 10,000,000 accesses of the stream from
seed 1, with the card compared whole over the debug link every million.

| | `6502-1mhz` | `6502-2mhz` | `fastest` |
|---|--:|--:|--:|
| directed scripts | 6 of 6 | 6 of 6 | 6 of 6 |
| stream accesses (with the scripts) | 10,000,595 | 10,000,595 | 10,000,595 |
| reads | 1,726,249 | 1,726,249 | 1,726,249 |
| reads compared with `Video.ts` | 1,475,718 | 1,475,718 | 1,475,718 |
| **reads wrong** | **0** | **0** | **0** |
| whole-card comparisons exact | 16 of 16 | 16 of 16 | 16 of 16 |
| strobe, sample point | 500 ns, 562.5 ns | 250 ns, 187.5 ns | 250 ns, 187.5 ns |
| mean spacing of accesses | 6.1 µs | 5.5 µs | 3.7 µs |
| time | 714 s | 719 s | 709 s |

And what the card counted over each profile:

| | `6502-1mhz` | `6502-2mhz` | `fastest` |
|---|--:|--:|--:|
| writes taken | 8,274,346 | 8,274,346 | 8,274,346 |
| reads taken | 1,726,249 | 1,726,249 | 1,726,249 |
| stale data reads | 0 | 0 | 0 |
| stale status reads | 549 | 487 | 523 |
| coincident accesses | 0 | 0 | 0 |
| FIFO overruns, write and read | 0, 0 | 0, 0 | 0, 0 |
| staging waits | 0 | 0 | 0 |
| bus interrupt, longest | 418 cycles | 418 cycles | 415 cycles |

The card took exactly the writes and reads the Nano made, 8,274,346 and
1,726,249 — every access arrived, and none twice. The uncompared reads are
status reads of registers that move with the raster; 2,779 `FONT` commands a
profile that would have loaded font `$00` were sent to a reserved register
instead ([Differences](#differences-from-the-plan)).

Every command form was in it: register writes to all 128 registers through both
pairs, write and read addresses with `VBANK` at `$00`–`$03` and with its unused
bits set, strides at `$00`, `±$01`, `$7F`, `$80`, `$81`, `$FE`, `$FF` across
bank carries and the 64 KB wrap, half pairs abandoned by data accesses and by
status reads, register writes on one pair between the halves of a pair on the
other, and the two pairs' data ports read interleaved in both orders.

The run's exit status was 1, from its back-to-back check as it then stood,
which failed on any wrong read at any spacing. It now fails only at 4 µs, the
spacing the plan requires; the 2 µs and flat-out rows are measurements.

---

Back to back
------------

§4's tightest case is a run of data reads on one port: each must return the
prefetch the one before it fetched, so the card has between two reads to take
the first and restage. PLAN.md's risk 5 names it. `READ_RUN` reads one port 120
times at a set spacing; 100 runs at each spacing fill VRAM through one pair and
read it back through the other, alternating.

| Spacing | Measured | Reads | Wrong |
|---|--:|--:|--:|
| 4 µs, a 1 MHz 6502 | 4.05 µs | 12,000 | **0** |
| 2 µs, a 2 MHz 6502 | 1.98 µs | 12,000 | 70 (0.58%) |
| flat out | 1.16 µs | 12,000 | 306 (2.6%) |

**At 4 µs — a 1 MHz 6502's back-to-back `lda` — nothing is wrong.** At 2 µs and
at the Nano's flat-out 1.16 µs, reads come back stale: every wrong byte is the
byte the read before it returned, and the card's own stale-data count agrees
with the Nano's exactly.

Why is measured. Core 1's latch interrupt, which takes every line start at the
bus interrupt's priority, costs 25 cycles to enter, about 358 in `vdp_latch` and
about 190 to restage — up to 548 cycles, 1.6 µs. A read that lands in it waits
for it to finish before its handler runs, and a read 2 µs behind that one can
get there first. The rate fits: a latch every 63.6 µs, 1.6 µs long, catches
about one read in 40 at 1.16 µs and fewer as the spacing grows, gone by 4 µs.
Turning the display on, which lengthens the renderer's interrupts-off
publication, raised the rate at 2 µs from 57 to 72 in 12,000.

PLAN.md puts both at one priority on purpose: `vdp_latch` and the bus handler
both write status and the journal, and a bus access that preempted a latch half
way through a read-modify-write of `STAT0`, or between a `FONT` load's bulk
write and its place in the journal, would split the bus and render copies. So
this phase does not change it. What would make 2 µs safe is for Phase 13 to
weigh: a shorter latch, a bus interrupt allowed to preempt the latch with the
latch's writes made safe for it, or a read program that can serve a second data
read on its own from a byte staged ahead.

---

Stale status, and what a stale read acknowledges
------------------------------------------------

A status read is answered from a byte staged before it (§6), so it can lag the
card. Over 30,001,785 accesses the card counted 1,559 (549, 487 and 523 at the three profiles) status reads served
a byte it had since moved past — a latch had moved `STAT2` or `STAT3`, or set a
flag, between the restage and the read. §6 calls `STAT3` b1 advisory and says
nothing finer than a display line should be timed from status; how stale, and
how often, is Phase 13's to measure.

What such a read does is this firmware's decision. §6 says a read of `STAT0`
clears its flags; a read that was served a byte without `F`, because `F` set
after the byte was staged, has not shown `F` to anyone. Clearing it would lose
the event, where §6 is at pains that the flags are sticky and that a program
looking once a second still sees what happened in between. So a stale `STAT0`
read clears the flags it showed, with what details each and the `STAT1` latch
each stands for, and leaves the rest; a stale `STAT1` read clears the latches it
showed. A read served exactly what the card holds is §6's read, unchanged. The
unit tests hold both. PLAN.md records it among the decisions SPEC.md may want to
state.

---

RST
---

`vdpctl reset-pin --trials 50` (`docs/results/phase-11/reset-pin.txt`). Each
trial starts from an RST pulse and reads the card's VRAM for the reference,
then, over the bus:

- display on, the vblank interrupt enabled, other registers and both `STATSEL`s
  at random, `L1PAT` at `$2000`; 64 bytes over the font and 32 over the palette
  window; pointers left mid-VRAM with `VINC` at `+1`, `-1` or `+64`;
- 40 ms later — two vertical blanks — `/INT` must be asserted;
- then **one** Nano batch: a `FONT` command loading font `$00` into layer 1, a
  payload left on pair A, a 100 µs RST pulse, and straight after it `STAT0`,
  `STAT1`, `STAT7` and all eight collision-map registers read;
- 60 ms later, frames on, `/INT` must be released, and VRAM, the registers and
  both ports must be `Video.ts`'s after `reset(false)`, and the snapshot's
  pending loads, latches and sprite status clear.

All 50 did. Everything §15 names: the registers at their reset values, both
pairs at pointer 0, read direction, prefetch 0, flip-flop clear and `STATSEL`
0; the palette back at `$FC00`–`$FDFF`; `$0800`–`$0FFF` byte for byte
`fonts/cp437-6x8.bin`; the pending `FONT` load cancelled, `$2000` untouched
frames later; the rest of VRAM as it was; every flag, `STAT7`, the map and
every latch clear; `/INT` released.

A negative control — the same trial with the Nano's pulse replaced by a wait —
fails at once on every count: `STAT0` reads `$BF`, `/INT` stays low, 1,584
bytes of VRAM and most registers differ. The check can see what it claims to.

---

Strobe and hold margins
-----------------------

`vdpctl sweep`, Phase 9's own test, on this firmware:

```
  width  0:ok  1:ok  2:ok  3:ok  4:ok   least that works: 250 ns strobe
  hold   0:ok  1:ok  2:ok  3:ok  4:ok   least that works: 0 ns hold
  setup  0:ok  1:ok  2:ok  3:ok  4:ok   least that works: 0 ns setup
  gap    0:ok  1:ok  2:ok  3:ok  4:ok   least that works: 0 us apart
```

The same as the stock firmware's: nothing the harness can produce fails. The
stream's own reads are sampled at that floor at `6502-2mhz` and `fastest`, on
all four ports, with `/CSR` held low for only 312 ns — a shorter strobe than
the sweep's.

---

The harness had to get faster
-----------------------------

A 10⁷-access run cannot go one access to a round trip: at 4 ms each it would
take eleven hours a profile. `SCRIPT` carries 120 accesses a packet and compares
reads on the Nano, which brings a run to about 14,000 accesses a second.

It then turned out that the Nano itself was the slowest thing on the bus. Its
first `SCRIPT` loop spent 133 cycles an access, 8.3 µs — a 6502 at 1 MHz reads
twice as fast — most of it in decoding: `op >> 6` compiled to a six-step shift
loop, and the port to a multiply. Rewritten, with the strobes kept as the exact
instruction sequences Phase 9 calibrated, the timings in registers, the drivers
left on between writes and a specialised loop for `fastest`, it runs at 3.5 µs
flat out. `READ_RUN`, a hand-counted loop for one port, reaches 1.125 µs, and 2
and 4 µs exactly with padding.

The profiles' `gap` became a period in `SCRIPT`, paced by Timer 1 from one
strobe to the next, as a CPU's instruction timing spaces its accesses; Phase 9's
block commands keep it as a pause. A paced profile cannot run faster than its
loop, so in the stream `6502-1mhz` accesses are 6.1 µs apart, `6502-2mhz` 5.5 µs and
`fastest` 3.7 µs: all at least as far apart as the profile says, and the tight
spacing is `READ_RUN`'s to test.

---

Done when
---------

PLAN.md's Phase 11:

| Check | Result |
|---|---|
| 10⁷ random port operations over both ports — every command form, `VBANK` and `VINC` edge cases, both read orders — through the Nano with no read mismatch, at each of the three profiles | ✅ 10,000,595 accesses at each profile, 1,475,718 reads compared at each, **none wrong**; 48 of 48 whole-card comparisons exact |
| back-to-back reads 4 µs apart return correct prefetch bytes | ✅ 12,000 reads at 4.05 µs, both pairs, none wrong. At 2 µs, 1 in 170 is stale ([above](#back-to-back)) |
| strobe and hold margins are no worse than Phase 9's baseline | ✅ the same: nothing the harness can produce fails |
| a `/RESET` pulse yields §15's state, confirmed by `SNAPSHOT`, with `$0800`–`$0FFF` the built-in font (`VRAM`) | ✅ 50 of 50, from a scrambled card with a `FONT` load pending; `SNAPSHOT` and `VRAM` agree with `Video.ts` after its reset, and `$0800`–`$0FFF` is `fonts/cp437-6x8.bin` |
| no FIFO overruns | ✅ none, write or read, over 30 million accesses, the RST trials and the sweep; nor any coincident access or staging wait |
| the host suite passes, the firmware presets build | ✅ 28 tests, and 21 under ASan and UBSan; all 364 Jest tests against the core; `pico2`, `pro-debug` and `pro-release` build with no warnings |

---

Differences from the plan
-------------------------

1. **The read program is a jump table, not a loop.** PLAN.md section 3 set the
   constraints — four bytes staged in one word, MODE and MODE1 sampled with
   `in pins, 2`, pin directions from a constant — and left the program to this
   phase. All three hold. What the plan could not know is that choosing the
   byte had to fit inside the debounce to meet Phase 9's baseline.
2. **The handler pushes back the byte it served, not only which port.** The
   plan's handler "advances that port's prefetch or acknowledges its status".
   It does, but with what the pins actually returned, which is what makes a
   stale status read safe.
3. **The directed scripts are a small language, and the stream is generated.**
   The plan's `tests/bench/` holds "port conformance scripts drawn from the fuzz
   corpus, with expected reads from the reference replay". The six scripts are
   there as `.bus` files; the 10⁷-access stream is drawn from `tools/fuzz.mjs`'s
   bus scope at run time from a seed rather than stored, and every expected read
   comes from `Video.ts` as the accesses are played. Neither stores expected
   values, because the card's VRAM, which RST leaves undefined, is read over the
   debug link at the start of each run and given to the reference.
4. **"Both read orders"** is taken as the two pairs' data ports read
   interleaved one way round and then the other, with status reads between, in
   the stream and in `read-orders.bus`.
5. **`FONT` is in the stream only when it names a reserved font.** A load of
   font `$00` lands at the next vertical blank, which an untimed run cannot
   place; the RST check runs one deliberately, where the timing is its own.
6. **The harness grew two commands and got faster** ([above](#the-harness-had-to-get-faster)).
   The plan had the script runner arrive with Phase 13; the part of it this
   phase needed, a batch of accesses compared on the Nano, arrived here.
7. **2 µs is measured, not required,** and does not pass. The plan's criterion
   is 4 µs.

---

For later phases
----------------

- **Phase 12** plays the traces through `SCRIPT`: a trace's accesses, 120 to a
  packet, with its reads compared on the Nano, exactly as the stream is here.
  The reference is already the trace's recorded reads, so only the settle
  points and the snapshot between them are new.
- **Phase 13** has three things from this phase. The 2 µs back-to-back reads,
  above, and what the latch interrupt costs a read waiting behind it —
  `latch_isr_max` under the load run is now also a bus latency. The card's
  stale-read counts, which are the raw material for how fresh the status byte
  is (Still Open 4). And `SCRIPT`'s controls, which are where `/INT` waits,
  timestamps and loops go.
- **The machine.** An AC6502 at 1 MHz is inside everything measured here. At
  2 MHz, unrolled `lda VC_DATA` runs would need the Phase 13 remedy first.

---

Reproducing
-----------

The bench is `docs/BENCH.md`, with the Nano now wired to all four ports.

```sh
cmake --workflow --preset host          # 28 tests, test_served among them
cmake --preset pro-debug && cmake --build --preset pro-debug
export PICOVDP_PRESET=pro-debug
node tools/vdpctl.mjs flash
(cd bench/nano && pio run -t upload)    # harness firmware 2: SCRIPT and READ_RUN

node tools/vdpctl.mjs bus
node tools/vdpctl.mjs conformance --timing all --ops 10000000 --out conformance.json
node tools/vdpctl.mjs reset-pin --trials 50
node tools/vdpctl.mjs sweep
node tools/vdpctl.mjs stats
```

`conformance` needs a 6502-EMULATOR checkout with `npm run build:cli` run in it,
for `Video.ts`. Raw results are in `docs/results/phase-11/`.
