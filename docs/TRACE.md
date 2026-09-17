VDP Port Traces
===============

**Format version 1.** How a golden fixture's traffic to the video card is
recorded, so the card can be held to the fixture's goldens with no 6502
attached (PLAN.md section 4).

A **trace** is everything a program did to the card's four ports, in order,
with the tick each thing happened on. It also holds what the card did by itself
in between: line starts and `/INT`. And it marks the checkpoints the goldens
were captured at. Replayed into a card that is fed only the program's side, a
trace must reproduce the card's side line for line and every golden byte for
byte. That is what makes it a faithful copy of the fixture, as far as the card
can tell.

| | |
|---|---|
| **Written by** | `6502-EMULATOR`: `scripts/record-traces.mjs`, through `Video.observer`, during the same run the goldens are captured from |
| **Reference reader** | `6502-EMULATOR/src/tests/goldens/traces.js`, and its CPU-less replay `scripts/replay-trace.mjs` |
| **Read here by** | `tools/lib/trace.mjs` (written from this document), `tools/sync-oracle.mjs`, `tools/fuzz.mjs`, `host/replay` (a reader of its own, in C), `tools/lib/inject.mjs` (`vdpctl inject`); later `vdpctl replay` |
| **Pinned here in** | `tests/oracle/<fixture>/<fixture>.vdpt.gz`, by `tools/sync-oracle.mjs` only |

In this document, **§** means a section of SPEC.md.

---

Contents
--------

1. [The File](#1-the-file)
2. [Time](#2-time)
3. [Events](#3-events)
4. [Checkpoints](#4-checkpoints)
5. [What a Reader Derives](#5-what-a-reader-derives)
6. [Replaying a Trace](#6-replaying-a-trace)
7. [Rules a Reader Checks](#7-rules-a-reader-checks)
8. [Versions](#8-versions)

---

1. The File
-----------

A trace is **gzip-compressed UTF-8 text**, one record a line, lines ending in
LF. It is named `<fixture>.vdpt.gz`. Read it with anything that reads gzip:
`gzip -dc bios.vdpt.gz | less`.

Text, because every consumer can parse it in a few lines, a person can read
it, and gzip compresses the long runs of status polls it is mostly made of.
The five fixtures hold 2.6 million events in 1.5 MB.

The file has three parts:

```
vdpt 1                                            magic and format version
fixture bios                                      header: key, space, value
emulator 65b67d1d1c7fb26dcea069d216f8a3ae1fdf0e37
frequency 1000000
---                                               end of header
0 X cold 24                                       events, one a line
64 L 25 1
…
63 C scroll 8000000 frame=479 settle=6056 window=0 class=static
end 131819                                        footer: the number of events
```

**Header.** The first line is `vdpt` and the format version. Then `key value`
lines, until a line that is exactly `---`. A reader must ignore keys it does
not know. Version 1 always has these three:

| Key | Value |
|---|---|
| `fixture` | the fixture's name, as in `6502-EMULATOR/src/tests/goldens/fixtures.js` |
| `emulator` | the emulator commit the trace was recorded at, with `-dirty` appended if the tree had other changes. A fuzzer's trace says `fuzz` |
| `frequency` | PHI2 in Hz: the tick rate. Every fixture runs at 1,000,000 |

**Footer.** The last line is `end` and the number of event lines. A file
whose footer is missing or disagrees is truncated.

---

2. Time
-------

The clock is the **tick**: one PHI2 cycle, as the card's `tick()` counts them.
Tick 0 is the cold reset that starts every trace. In the golden fixtures the
machine is built and reset at cycle 0, so a tick is also the machine's cycle
count, and a checkpoint records both to prove it.

Every event line starts with a **delta**: the ticks since the previous event,
in decimal. The first event's delta is from tick 0. An event's **absolute
tick** is the sum of the deltas up to and including its own.

An event at absolute tick *T* happens **after the card's *T*-th tick and before
its (*T*+1)-th**, and events on the same tick happen in file order. Two cases
matter:

- **A line start at *T*** is caused by the *T*-th tick itself. `Video.ts`
  advances a line when a per-tick accumulator passes `f / 60 / 262` cycles —
  63.61 at 1 MHz — so line starts fall 63 or 64 ticks apart.
- **A read or write at *T*** is part of the instruction that begins in cycle
  *T*+1. The emulator's CPU makes all of an instruction's bus accesses in its
  first cycle, before that cycle's tick reaches the card. So a program's port
  access always lands *between* ticks, and never splits one from its line
  start.

The raster is not continuous in the emulator the way it is on the PRO: 262
equal lines at exactly 60 Hz, and no odd VGA line. Consumers that address
events by **frame and line** instead of by tick (section 5) are unaffected by
that, which is why a trace carries both.

---

3. Events
---------

`<delta> <type> <fields…>`, separated by single spaces. Numbers are decimal,
except byte values, which are two lowercase hex digits.

Events are either the **program's** (what a replay feeds in) or the **card's**
(what a replay must produce by itself):

| Type | Fields | Side | Meaning |
|---|---|---|---|
| `X` | `cold` or `warm`, *screen line* | program | A reset (§15). `cold` is power-on. It zeroes the tick count, puts the raster at *screen line*, and begins that line: a cold reset's line start is part of the `X`, with no `L` of its own. `warm` is `RST`: the raster runs on, and *screen line* is the line it is on |
| `W` | *port*, *value* | program | *value* was written to *port*. Ports are A1:A0 (§4): 0 data A, 1 command/status A, 2 data B, 3 command/status B |
| `R` | *port*, *value* | program | *port* was read and returned *value*. The program's side is the read; the value is the card's answer, which a replay compares |
| `L` | *screen line*, *display line* | card | A screen line began (§3). *Screen line* counts from the top of the frame, 0–261. *Display line* is what the line is numbered in the geometry in effect as it began: what `STAT2` reads, and what `IRQLINE` compares against |
| `I` | `0` or `1` | card | `/INT` changed: `1` asserted, `0` released (§14). Sampled after every other event and written only when it differs. It is released after a cold reset |
| `C` | *name*, *cycles*, then `key=value` fields | — | A checkpoint: a golden was captured here. Section 4 |

`/INT` is the card's pin: asserted while any latched interrupt source is still
enabled, `STAT1` ≠ 0. An `I` follows the event that changed it, on the same
tick: an `L` that latched a source, a `W` that enabled or disabled one, an `R`
of `STAT0` or `STAT1` that acknowledged one.

Real lines, from the `wizardslab` trace. The vertical-blank interrupt is raised
as display line 192 begins — screen line 216 in Graphics I's 192-line geometry —
while the program is still writing VRAM:

```
15 W 0 63
12 L 216 192
0 I 1
3 W 0 00
```

and acknowledged by a status read 43 screen lines into the next frame, which
returns `$D0`: `F` and `OVF` set, with sprite 16 the first dropped:

```
64 L 43 19
20 R 1 d0
0 I 0
43 L 44 20
```

**Version 1 holds exactly one cold reset, as its first event, at tick 0.** A
cold reset zeroes the tick count, so the ticks between a later event and a
second cold reset would be lost. Warm resets may appear anywhere after it; none
of the fixtures has one.

---

4. Checkpoints
--------------

```
63 C ok 1000000 frame=59 settle=5028 window=0 class=static
```

*Name* is the checkpoint's name in its fixture, and its golden files are
`<name>.idx.bin`, `.vram.bin`, `.json` and `.png`. *Cycles* is the machine's
cycle count when the golden was captured, and always equals the event's
absolute tick. A checkpoint happens between ticks, like a read: the capture
saw every tick up to *cycles* and none of the instruction that follows.

**What a golden holds, and when.** A checkpoint's VRAM and JSON are the card as
it stands at the checkpoint. Its index frame is not: it is the **last frame
presented before the checkpoint**. These are different moments. `vdp-layers`,
for one, writes the next frame's scroll registers in vertical blank, after its
frame is presented and before the checkpoint.

**Frames.** Screen line *S* of the frame is built from the state as screen
line *S* − 1 begins (§3). So row 0 of frame *k* is built as frame *k* − 1's
screen line 261 begins, row 239 as frame *k*'s screen line 238 begins, and the
frame is presented then. Those two line starts are the frame's **first-row
latch** and **last-row latch**.

The replay appends four fields to each checkpoint, in this order:

| Field | Meaning |
|---|---|
| `frame` | the number of the golden frame. Frame 0 is the one in progress at the cold reset; frame *k* begins at the *k*-th `L 0` |
| `settle` | the **settle point**: how many `R` and `W` events come before the golden frame's first-row latch. Play that many, and the state the frame was built from is complete |
| `window` | how many `R` and `W` events fall between the first-row latch and the last-row latch |
| `class` | `static` or `dynamic` |

**Class.** A checkpoint is **static** when its golden frame is a function of the
state at its settle point alone. Replay the first `settle` operations, apply
nothing more, and let the card run until frame `frame` is presented. If that
frame is the golden, byte for byte, the checkpoint is static; otherwise it is
**dynamic**, and only a replay timed to the line reproduces it.

This is decided by running it, not by counting operations in the window. A
status read in the window changes no picture, and neither does a write the
remaining rows never read. Fourteen of the eighteen goldens have 1,534–1,690
operations in their window, every one a status poll on port A, and all eighteen
are static.

What the class is for: `vdpctl replay` (Phase 12) plays a static checkpoint
through the bus with no timing at all. It pauses at the settle point, takes the
next complete frame, then plays on to the checkpoint for VRAM and registers.

A checkpoint line without these fields is a trace the replay has not annotated.
`record-traces.mjs` never writes one, and `sync-oracle.mjs` refuses one.

---

5. What a Reader Derives
------------------------

None of these is stored. Each follows from the rules above, and
`tools/lib/trace.mjs`'s `events()` yields them with every event:

| Quantity | How |
|---|---|
| absolute tick | the running sum of deltas |
| frame | 0 at the cold reset; +1 at each `L 0` |
| screen line | the `X cold`'s; then each `L`'s |
| display line | each `L`'s. Before the first `L`, the cold reset's screen line less 24: a cold start's geometry is always Compact's (§9, §15) |
| tick in line | the event's absolute tick less that of the latest `L` or `X cold` |
| operation index | the count of `R` and `W` events before it. `settle` is measured in these |

**Addressing by frame and line.** An injection executor (`vdpctl inject`,
Phase 8; docs/DEBUGLINK.md section 4) applies each operation after the latch of
its (frame, screen line) and the row that latch starts, as if
from the bus, and needs no clock. The trace's order still decides which
operations share a line and which side of a line start they fall. Screen lines
are the address, not display lines, which move when the geometry changes (§3).

**Tick in line** is what `STAT3` b1 (horizontal blanking, §6) depends on in the
emulator. Only an executor that reproduces `Video.ts`'s accumulator can match
those reads. On silicon the bit is advisory, and the bench does not compare it.

---

6. Replaying a Trace
--------------------

The reference replay, `6502-EMULATOR/src/tests/goldens/traces.js`'s
`replayTrace`:

1. Make a card, attach a recorder that writes events as section 3 says, and
   reset it cold. That produces the first line, `0 X cold 24`.
2. For each event, in order:
   - **`X`, `W`, `R`, `C`** — the program's side. Tick the card until its tick
     count is the event's absolute tick. No event of the card's may come out
     while it does. Then reset, write, read or capture.
   - **`L`, `I`** — the card's side. Tick until the card produces an event.
   - Every event the card's recorder has produced so far must equal the
     trace's, line for line, ignoring a `C` line's `key=value` fields. A read
     that returned another value is a line that differs.
3. At each `C`, capture what a golden holds and compare it with the golden.

A replay that reaches the end of the trace with no difference, and matches all
of a fixture's goldens, proves the trace complete.

An executor that has no tick clock plays the program's events in order,
addressed as section 5 says, and compares what it can:

| Executor | Line starts | Reads | `/INT` | Frames |
|---|---|---|---|---|
| reference replay (`Video.ts`) | exact | all | exact | every checkpoint |
| host adapter (`core/`) | its own accumulator, as `Video.ts`'s | all | exact | every checkpoint |
| injection (firmware) | from the raster, by (frame, line) | all but `STAT3` b1 and `STAT5` | by line | every checkpoint, by snapshot marker |
| bus replay (the Nano) | none — untimed | VRAM, `STAT4`–`STAT6` | — | static checkpoints, from the settle point |

---

7. Rules a Reader Checks
------------------------

`tools/lib/trace.mjs` refuses a trace that breaks any of these, naming the
event:

1. The magic is `vdpt 1`. The header has `fixture`, `emulator` and `frequency`,
   and ends with `---`. The footer is `end` and the number of events.
2. Every event starts with a decimal delta, and has a known type with exactly
   its fields. Ports are `0`–`3`, and values are two lowercase hex digits.
3. The first event is `X cold` at tick 0, and there is no other `X cold`.
4. `L` screen lines run in order, each one more than the last and wrapping
   from 261 to 0. The first follows the cold reset's screen line. Display lines
   are 0–261.
5. `I` alternates: never the level it already was. After a cold reset, `/INT`
   is 0.
6. A `C`'s *cycles* is its absolute tick, and every trailing field is
   `key=value`.

`sync-oracle.mjs` also requires, before it pins a trace, that the trace's
checkpoints are the fixture's in order, each with `frame`, `settle`, `window`
and `class`, and that each `C`'s *cycles* is its golden JSON's `cycles`.

---

8. Versions
-----------

The version is the number on the first line. A change that an existing reader
would misread — a field moved or reinterpreted, a new event type, a second
cold reset — is a new version, specified here. A new header key or `C` field is
not: readers ignore what they do not know. Traces are always re-recorded from
the emulator, never converted.

| Version | Change |
|---|---|
| 1 | First. Phase 2 |
