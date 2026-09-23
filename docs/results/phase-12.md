Phase 12 — Traces through the bus
=================================

**Status:** done on 2026-09-23. Every criterion in PLAN.md's Phase 12 is met.
The first full run was not green, and the fault was the host's rather than the
card's: [A lost answer, retried](#a-lost-answer-retried).

The oracle, played through the pins. Phase 10 reproduced every golden by
injection, which applies each operation inside the firmware; here the same
traces' operations arrive as `/CSW` and `/CSR` strobes from the Nano, with no
timing at all, on the PICO9918 PRO running this repository's `pro-debug` build
`71a2245`.

**The headline.**

- **All eighteen checkpoints reproduce exactly through the bus, at each of the
  three profiles** — 54 replays. Each checkpoint's index frame, taken from the
  card after its settle point, is the golden's in all 76,800 pixels. At the
  checkpoint, all 64 KB of VRAM and all 128 registers are the golden's. At each
  fixture's last checkpoint, VRAM read back **through the bus** is the golden's
  as well.
- **Every compared read was right** and **every access arrived once**: at every
  checkpoint the card had taken exactly as many accesses as the Nano had made.
  There was no FIFO overrun, no coincident access and no stale data read, across
  6,419,319 operations — 2,139,773 at each profile.
- **The traces barely read.** Of those operations, 98% are status polls, and
  the only reads an untimed replay can compare are the BIOS's detection probe
  (`STAT4`, `STAT5`, `STAT6` once each). No fixture ever reads VRAM through a
  data port. So the replay also reads all 64 KB back through the bus
  ([below](#what-can-be-compared-untimed)).
- **The checks can fail.** One bit of one write changed shows up in VRAM both
  ways. Frames taken one write early differ from their goldens in 11 of 18
  checkpoints; in the other seven, that write changes nothing on screen
  ([Negative controls](#negative-controls)).
- **No checkpoint is dynamic.** Phase 2 found all fifteen static, draft 0.5's
  three `vdp-font` checkpoints are static too, and the oracle has not been
  re-synced since. So no injection result has to stand in for a bus result.

Contents: [What was built](#what-was-built) ·
[The replay](#the-replay) ·
[What can be compared untimed](#what-can-be-compared-untimed) ·
[FONT, the one timed operation](#font-the-one-timed-operation) ·
[A lost answer, retried](#a-lost-answer-retried) ·
[Negative controls](#negative-controls) ·
[Done when](#done-when) · [Differences from the plan](#differences-from-the-plan) ·
[For later phases](#for-later-phases) · [Reproducing](#reproducing)

---

What was built
--------------

- **`tools/lib/bus-replay.mjs`**, behind **`vdpctl replay <fixture|all|trace>
  [checkpoint ...] [--timing NAME|all] [--out FILE.json] [--frames DIR]`**:
  PLAN.md section 4's bus executor. `--frames` keeps any frame that differs, as
  an index file.
- **`Nano.script` and `Nano.readBlock` take `{ retry }`.** Both still retry by
  default, as Phase 11's callers expect. The replay turns it off
  ([below](#a-lost-answer-retried)).
- `docs/BENCH.md` describes the replay and adds it to the bring-up order.

No firmware changed, and neither did the Nano's harness: the replay is a
`SCRIPT` client, exactly as Phase 11's "For later phases" foresaw.

---

The replay
----------

For each fixture, and each profile:

1. **The cold reset.** `RESET` with power-on, over the debug link. The trace
   begins with `X cold`, and a cold start's VRAM is zeroed (PLAN.md section 4).
   The RST pin performs §15, which leaves VRAM as it was, so it cannot stand in.
2. **The operations,** in the trace's order, 120 to a `SCRIPT`, at the
   profile's strobes and spacing. The Nano compares each read that carries an
   expected value, and reports those that differ.
3. **At each settle point** — the operation count before the golden frame's
   first-row latch — the replay stops, waits 20 ms, and takes the next frame to
   start with `SNAPSHOT`. The wait is well over a line, so every row of that
   frame is latched after the last operation (§3). The frame is compared with
   the golden `.idx.bin`, and must have no late rows.
4. **At each checkpoint,** the card's count of accesses taken must equal the
   Nano's. Then VRAM over the link is compared with the golden `.vram.bin`, and
   the 128 registers — `$02`–`$06` read through their aliases — with the
   golden JSON's.
5. **After the fixture's last checkpoint,** all 64 KB are read back through the
   bus on pair B — `VINC` 1, `VBANK` 0, a read address of `$0000`, then 274
   blocks, the pointer carrying through the banks (§4) — and compared with the
   same golden. This writes `VBANK`, `VINC` and pair B's state, which is why it
   comes last.

`Video.ts`, played the same operations with no ticks, serves only as the
decoder. It says which status register each status read selects, and which
command writes are `FONT`. Its registers follow the program exactly, since none
of them moves with time. Its VRAM does not after a `FONT`, and is never used.

`vdpctl replay all --timing all` (`docs/results/phase-12/replay.txt` and
`.json`):

| Fixture | Operations | Checkpoints | `6502-1mhz` | `6502-2mhz` | `fastest` |
|---|--:|--:|--:|--:|--:|
| `bios` | 6,055 | 3 | exact, 4.4 s | exact, 4.3 s | exact, 4.1 s |
| `wizardslab` | 1,002,238 | 4 | exact, 74.9 s | exact, 74.2 s | exact, 73.6 s |
| `vdp-modes` | 393,122 | 4 | exact, 32.0 s | exact, 31.7 s | exact, 31.4 s |
| `vdp-layers` | 509,139 | 4 | exact, 40.1 s | exact, 39.7 s | exact, 39.3 s |
| `vdp-font` | 229,219 | 3 | exact, 20.3 s | exact, 20.0 s | exact, 19.7 s |

"Exact" is everything in steps 2–5: the frame, VRAM both ways, the registers,
the access count, and every compared read. What the card counted, per fixture
at each profile:

| | `6502-1mhz` | `6502-2mhz` | `fastest` |
|---|--:|--:|--:|
| stale data reads | 0 | 0 | 0 |
| stale status reads (all five fixtures) | 233 | 167 | 226 |
| coincident accesses | 0 | 0 | 0 |
| FIFO overruns, write and read | 0, 0 | 0, 0 | 0, 0 |
| bus interrupt, longest | 345 cycles | 344 cycles | 346 cycles |

The stale status reads are the programs' vertical-blank polls, answered from a
byte staged before a latch moved `STAT0` (Phase 11, "Stale status"). None is
compared, and none changes what the card holds.

`STAT0` is recorded at each checkpoint but not compared. At fourteen of the
eighteen the card's reads `$80` where the golden JSON has `$00`: in the
emulator the program had just cleared F by polling, and here, 20 ms after the
last poll, a vertical blank has set it again. `STAT0` moves with the raster
(§6); injection compares it (Phase 10).

---

What can be compared untimed
----------------------------

PLAN.md asks the replay to check VRAM reads and the status reads that do not
depend on timing, `STAT4`–`STAT6`. The traces hold 6,316,758 reads over the
three profiles, and almost none of them can be compared:

| Read | In the traces | Compared |
|---|--:|---|
| data port | **0** | — no fixture reads VRAM through the bus |
| `STAT4`, `STAT6` | 1 each, in the four fixtures that boot the BIOS | against the trace's `$AC` and `$BF` |
| `STAT5` | 1, likewise | against `INFO`'s version, `$00`, not the trace's `$05` (PLAN.md section 4) |
| any other status register | all the rest | no: they move with the raster |

`vdp-font` never calls `KernalInit`, so it has no probe, and compares no read
at all. The rest of every trace is programs waiting for vertical blank.

So "checks VRAM reads" would be empty for this oracle. The replay gives it
substance by reading VRAM back through the bus itself (step 5): 64 KB through
the read program, five times a profile, each byte against the golden. That is
the same path Phase 11's back-to-back and stream reads took, now ending at a
golden instead of at `Video.ts`.

---

FONT, the one timed operation
-----------------------------

An untimed replay cannot place anything that happens at a line start. §7's
`FONT` load lands at the next vertical blank, so any operation the program made
between writing `FONT` and seeing the load land would, in the replay, fall on
whichever side of the card's vertical blank the Nano's pace put it.

A scan of the traces, run before the replay was written, found four loads of
font `$00`. Between each and its landing line, the program made nothing but
status polls — 1,020 to 1,832 of them — and, in `bios`, one `STATSEL` write:

| Fixture | Written at operation | Frame, line | Lands at frame, line | In between |
|---|--:|---|---|---|
| `bios` | 53 | 18, 218 | 19, 216 | 2 command writes, 1,832 status reads |
| `vdp-modes` | 36 | 0, 71 | 0, 216 | 1,020 status reads |
| `vdp-font` | 112,681 | 63, 246 | 64, 216 | 1,635 status reads |
| `vdp-font` | 225,659 | 127, 247 | 128, 216 | 1,632 status reads |

Every program writes `FONT` and then waits for F, as §7 tells software to. So
the replay plays the polls, then holds back the operation after the landing
line until a snapshot shows no load pending. All four holds, at every profile,
found the load landed. The replay also reports any data access between a
`FONT` and its landing, which would make that fixture depend on the raster.
None exists.

The holds turned out not to be what made these checkpoints pass. With them
removed (control (a) below), `bios`, `vdp-modes` and `vdp-font` still
reproduce: a thousand polls take several frames through the harness, so the
load lands while they play. The holds stay, so that the replay's correctness
does not rest on the harness being slow.

---

A lost answer, retried
----------------------

The first full run (`docs/results/phase-12/first-run.txt`) reproduced 53 of the
54 replays. `bios/scroll` at `6502-1mhz` had VRAM exact over the link, and 938
bytes wrong in the readback through the bus, the first at `$FB14`.

The card said why. It had taken 67,613 reads in that run, and 67,373 at the
other two profiles: 1,837 of the program's, 65,536 of the readback, and 240
more — one `READ_BLOCK` read twice. The run was also 4 s longer than the
others, which is `Nano.request`'s timeout. One of the Nano's answers had been
lost, and `request` retried the command. For `PING`, `PROFILE` or register
writes, that is harmless. For a block read, which moves the pointer, the retry
read the *next* 240 bytes, and everything after it came back 240 places late.
The same retry could replay a `SCRIPT` of 120 accesses twice.

So the replay never repeats an access blindly:

- **A `SCRIPT` whose answer is lost:** the replay reads the card's count of
  accesses taken over the debug link, which Phase 11 found exact. If the count
  has not moved, the batch never ran and is sent again. If it moved by the
  whole batch, the batch ran, and its reads went unchecked. That is reported,
  and fails the run if any of them was a compared read. Any other count stops
  the run.
- **A readback block whose answer is lost:** the replay points pair B at the
  block's own address again and reads it again.
- **At every checkpoint** the card's count must equal the Nano's, whatever
  happened on the way.

The recovery was exercised on purpose (`docs/results/phase-12/recovery.txt`).
A scratch runner dropped the answer to a `SCRIPT` after the Nano had run it,
failed another `SCRIPT` before sending it, and dropped the answer to a
readback block. The replay recovered all three, and `bios` reproduced
exactly. The second full run lost no answers.

Phase 11's conformance runner still uses the retrying default. Its whole-card
comparisons would catch a doubled batch as a state failure, so a lost answer
there costs a false failure rather than a false pass. None occurred in its 30
million accesses.

---

Negative controls
-----------------

A replay that cannot fail proves nothing. Three controls, each the replay
unchanged but for one alteration to its plan, made in a scratch copy
(`docs/results/phase-12/controls.txt`), at `6502-1mhz`:

| Control | What changed | Result |
|---|---|---|
| (a) | every `FONT` hold removed | passes — the polls after each `FONT` outlast a frame ([above](#font-the-one-timed-operation)) |
| (b) | bit 0 of the last data write before `parallax`'s settle point flipped | **fails**: VRAM differs at `$3013`, over the link and through the bus alike. The frame is exact: `$3013` is not on screen |
| (c) | each frame taken at the last write before its settle point | **fails** in 11 of 18: every `vdp-modes`, `wizardslab` and `vdp-font` frame differs, by 24 to 76,504 pixels. The last write before `vdp-layers`' and `bios`'s settle points changes nothing on screen |

---

Done when
---------

PLAN.md's Phase 12:

| Check | Result |
|---|---|
| `vdpctl replay` plays a trace's operations through the Nano, untimed | ✅ all five fixtures, 2,139,773 operations, at each of the three profiles |
| it checks VRAM reads, and status reads that are timing-independent (`STAT4`–`STAT6`) | ✅ every such read in the traces right — three per BIOS boot, as there are no data reads — and all 64 KB of VRAM read back through the bus at each fixture's end, exact |
| it pauses at each checkpoint's settle point, takes the next complete frame with `SNAPSHOT`, plays on to the checkpoint and reads VRAM and registers there | ✅ |
| every static checkpoint reproduces exactly through the bus — index frame from the settle point, VRAM and registers from the checkpoint | ✅ **18 of 18, at each profile: 54 of 54** |
| a checkpoint that becomes dynamic after a re-sync is listed, with its Phase 10 injection result standing for it | ✅ none: the oracle is unchanged since Phase 10, and all eighteen are static |
| the host suite passes | ✅ 28 of 28 tests; no firmware or core change |

---

Differences from the plan
-------------------------

1. **The cold start comes over the debug link.** The trace's `X cold` is
   `RESET` with power-on, which zeroes VRAM as the emulator's cold start does.
   RST, the only reset on the bus, performs §15, which leaves VRAM undefined.
   Everything after the cold start goes through the pins.
2. **`STAT5` is compared,** against the version `INFO` reports, rather than
   excluded. The plan's section 4 lists it among the known differences; it is a
   known value, so it can be checked.
3. **VRAM is also read through the bus,** because the traces make no data
   reads for the replay to check ([above](#what-can-be-compared-untimed)).
4. **`FONT` loads are held for,** the one place where an untimed replay would
   otherwise rest on luck ([above](#font-the-one-timed-operation)).
5. **Each fixture plays once per profile,** from one cold start through all of
   its checkpoints, rather than once per checkpoint. Every settle point in the
   oracle comes after the previous checkpoint. The replay refuses a trace in
   which one does not, rather than playing it wrong.
6. **All three profiles,** where the plan names none. At `6502-2mhz` and
   `fastest`, `SCRIPT`'s accesses are 5.5 and 3.7 µs apart (Phase 11), so none
   of this reaches the 2 µs back-to-back case that Phase 13 is to fix.

---

For later phases
----------------

- **Phase 13's release parity** asks `pro-release` to repeat Phases 11 and 12
  "using VRAM readback and capture in place of the snapshot". The replay's
  readback through the bus is that readback, and its per-checkpoint checks are
  the parts to swap. Two parts need the debug link, which a release build lacks:
  the cold start, which RST plus a 64 KB zeroing through the bus could replace,
  and the frame, which the capture replaces.
- **Any host command that moves the card must not be retried blindly.** The
  harness's `request` still retries by default. A caller that plays accesses
  should pass `{ retry: false }` and decide for itself, as the replay does.
- **The oracle has nothing that reads VRAM back.** A fixture whose program reads
  the card — a BIOS routine that scrolls in software, or a game that reads its
  map — would give both the bus replay and injection data reads to compare.
  That belongs to the emulator's goldens, not here.

---

Reproducing
-----------

The bench is `docs/BENCH.md`, as Phase 11 left it; no harness change.

```sh
cmake --workflow --preset host
cmake --preset pro-debug && cmake --build --preset pro-debug
export PICOVDP_PRESET=pro-debug
node tools/vdpctl.mjs flash

node tools/vdpctl.mjs bus
node tools/vdpctl.mjs replay all --timing all --out replay.json
```

`replay` needs a 6502-EMULATOR checkout with `npm run build:cli` run in it, for
`Video.ts` as the decoder. The full run takes about nine minutes. Raw results are
in `docs/results/phase-12/`.
