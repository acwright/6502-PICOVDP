Phase 2 — The oracle, exported
==============================

**Status:** done on 2026-09-13. Every check below passed.

**The headline.**

- **The goldens replay without a CPU.** Each golden fixture's port traffic is
  recorded as a trace: 2.4 million events, 1.3 MB compressed. Replayed into a
  bare `Video.ts`, every read, line start and `/INT` change comes out as
  recorded, and **all fifteen checkpoints match byte for byte**: index frame,
  VRAM and JSON.
- **All fifteen checkpoints are static.** Freeze the card at a checkpoint's
  settle point and it presents the golden frame anyway. So Phase 12 can
  replay every golden through the bus with no timing.
- **The C side is wired.** `npm run test:picovdp` runs all 342 of
  `Video.test.ts`'s tests against the empty core through the new Node-API
  adapter. 338 fail, as they should. The fuzzer finds its first divergence
  within 93 operations and minimises it to two.

Contents: [What was built](#what-was-built) · [The traces](#the-traces) ·
[Checkpoints](#checkpoints) · [Is the replay checking anything?](#is-the-replay-checking-anything) ·
[The observer's cost](#the-observers-cost) · [Video.test.ts against the core](#videotestts-against-the-core) ·
[The fuzzer](#the-fuzzer) · [Done when](#done-when) · [Differences from the plan](#differences-from-the-plan) ·
[Reproducing](#reproducing)

---

What was built
--------------

### In 6502-EMULATOR, on `v3-vdp`

Three commits, pushed:

| Commit | What |
|---|---|
| `bc605e7` | **Video: a port observer, and traces of the goldens for 6502-PICOVDP** |
| `4d9e328` | **Record the VDP port traces of the golden fixtures**, in a commit of their own |
| `8e40c0c` | **Docs: the oracle exported to 6502-PICOVDP** — the goldens README, README, PLAN.md, CLAUDE.md |

What the first commit adds:

| Part | What it is |
|---|---|
| `Video.observer` | optional hook, called after each port read or write, each line start the raster makes, and each reset |
| `Video.tickCount` | ticks since the card was made or cold-started |
| `src/tests/goldens/traces.js` (+ `.d.ts`) | shared by the scripts and the test, like `fixtures.js`. The TRACE.md reader and writer, the recorder, the CPU-less replay, and the class analysis |
| `runFixture(…, { beforeReset })` | the one moment a recorder can be attached before the cold start |
| `scripts/record-traces.mjs` | `npm run record:traces [-- --check]`. Writes `<fixture>.vdpt.gz` beside the goldens, only if the recorded run reproduces the goldens and the trace replays to them |
| `scripts/replay-trace.mjs` | `npm run replay:traces [-- <trace>…] [--out DIR]`. The reference executor |
| `jest.picovdp.cjs` | `npm run test:picovdp`: `Video.test.ts`, unchanged, with `src/core/IO/Video` mapped to the module `PICOVDP_ADDON` names |
| `VideoObserver.test.ts` | 4 tests, kept out of `Video.test.ts` because the C core runs that file too |
| `Traces.test.ts` (second commit) | 8 tests: recording disturbs no golden, the committed traces are current, and they replay to every golden with the class they state |

The emulator's `npm test` has 1,925 tests, up from 1,913, and all pass.
`npm run typecheck` passes, and `capture:goldens -- --check` moves nothing.

The `/Applications/6502 Emulator v3.app` copy was rebuilt from `8e40c0c`. The
2.6.9 app is untouched.

### Here

| Part | What it is |
|---|---|
| `docs/TRACE.md` | the trace format, version 1 |
| `tools/lib/trace.mjs` | a reader and writer written from TRACE.md rather than copied from `traces.js`, checking every rule of its section 7; a recorder |
| `tools/lib/emulator.mjs` | finds the emulator (`PICOVDP_EMULATOR`), its fixtures, its compiled `Video`, and the adapter |
| `tools/sync-oracle.mjs` | the pinned copy and its manifest; `--check` verifies it (CTest `oracle_pinned`) |
| `tests/oracle/` | the first sync, from `8e40c0c`: 64 files, 3.5 MB |
| `core/vdp.h`, `core/vdp.c` | section 3's interface over an empty core, built for the host and the RP2350 |
| `host/node/addon.c` | the core behind Node-API, one function per entry point. Built by CMake against the headers of the Node on `PATH`, with no node-gyp |
| `host/node/Video.cjs` | the adapter: `Video.ts`'s public surface, with the raster accumulator, `STAT3` b1 and the frame buffers in JavaScript and the card in C. It builds and colours frames through `vdp_build_line` and `vdp_expand_line`, the firmware's own path |
| `host/node/smoke.cjs` | CTest `node_addon_loads`: the binding is whole and the adapter presents frames |
| `tools/fuzz.mjs` | the fuzzer skeleton |

---

The traces
----------

`docs/TRACE.md` specifies them. In short: gzip-compressed text, one event a
line, each line starting with the ticks since the last one.

| Fixture | Events | Reads | Writes | Line starts | `/INT` changes | Compressed |
|---|--:|--:|--:|--:|--:|--:|
| `bios` | 232,995 | 3,681 | 8,916 | 220,394 | 0 | 44 KB |
| `wizardslab` | 1,159,492 | 994,751 | 6,347 | 157,199 | 1,190 | 680 KB |
| `vdp-modes` | 449,968 | 378,521 | 11,968 | 59,474 | 0 | 268 KB |
| `vdp-layers` | 586,410 | 497,858 | 9,948 | 78,599 | 0 | 336 KB |

Each trace also has its cold reset and its checkpoints. Nearly every read is a
status poll on port A, waiting for vertical blank. Port B is never used, and
only Wizards Lab takes an interrupt.

**A bus access lands between ticks.** The emulator's CPU makes all of an
instruction's bus accesses in its first cycle, before that cycle's tick
reaches the card. So an operation recorded at tick *T* comes after the *T*-th
tick's line start and before the next tick. It never splits a tick from its
line start.

**The card's ticks are the machine's cycles.** The recorder asserts it at
every checkpoint.

---

Checkpoints
-----------

| Fixture | Checkpoint | Cycles | Frame | Settle | Window | Class |
|---|---|--:|--:|--:|--:|---|
| `bios` | `ok` | 7,000,000 | 419 | 4,230 | 0 | static |
| `bios` | `screenful` | 10,680,000 | 639 | 4,623 | 0 | static |
| `bios` | `scroll` | 14,020,000 | 840 | 12,597 | 0 | static |
| `wizardslab` | `frame-60` | 1,000,000 | 59 | 90,458 | 1,576 | static |
| `wizardslab` | `frame-180` | 3,000,000 | 179 | 292,525 | 1,576 | static |
| `wizardslab` | `frame-300` | 5,000,000 | 299 | 494,406 | 1,576 | static |
| `wizardslab` | `frame-600` | 10,000,000 | 599 | 999,183 | 1,576 | static |
| `vdp-modes` | `text` | 600,000 | 35 | 57,638 | 1,688 | static |
| `vdp-modes` | `compact` | 1,650,000 | 98 | 166,651 | 1,687 | static |
| `vdp-modes` | `graphics` | 2,716,667 | 162 | 277,248 | 1,690 | static |
| `vdp-modes` | `full` | 3,783,334 | 226 | 388,462 | 1,689 | static |
| `vdp-layers` | `parallax` | 1,500,000 | 89 | 120,302 | 1,689 | static |
| `vdp-layers` | `scroll-bit8-l1` | 3,000,000 | 179 | 285,509 | 1,689 | static |
| `vdp-layers` | `occluded` | 4,000,000 | 239 | 395,656 | 1,690 | static |
| `vdp-layers` | `scroll-bit8-l0` | 5,000,000 | 299 | 505,794 | 1,689 | static |

*Frame* is the golden frame's number, and frame 0 is the one in progress at
the cold reset. *Settle* counts the operations before that frame's first row
was latched. *Window* counts the operations while its rows were being latched.

**The class is decided by running it.** PLAN.md section 4 defined a static
checkpoint as one with no operation in its window. By that rule, twelve of the
fifteen would be dynamic. Every operation in those windows turned out to be a
status read on port A (checked event by event), and a status read changes
nothing a frame shows. So the class is decided the way Phase 12 will use it:

1. Replay the first *settle* operations.
2. Apply nothing more, and tick until frame *frame* is presented.
3. Compare that frame with the golden.

All fifteen match. No checkpoint has a write in its window, so all fifteen are
also static under a stricter "no write in the window" reading.

---

Is the replay checking anything?
--------------------------------

A replay that passes is only evidence if it can fail. `bios`'s trace was
corrupted one way at a time and replayed:

| Corruption | Caught |
|---|---|
| one data-port read's value, one bit flipped | ✅ at that event: "`6 R 0 21`, the replay made `6 R 0 20`" |
| one data write's value changed | ✅ at all three checkpoints (VRAM and frame) |
| one line start deleted | ✅ at the next line start, whose delta and number are wrong |
| one checkpoint's `settle` off by one | ✅ against the analysis |
| the last write before `scroll`'s settle point deleted | ✅ every later event shifts |
| one write moved a tick later within its line | not caught — an equivalent mutant: no state or event changes |

`replay-trace.mjs --out` writes the replayed checkpoints' files. All 60
replayed from the pinned traces in `tests/oracle/` are byte-identical to the
pinned goldens, PNGs included.

---

The observer's cost
-------------------

`npm run bench` on the emulator before (`6cb0d0a`) and after (`bc605e7`), run
back to back on the same machine, as multiples of real time:

| Workload | 1 MHz before | 1 MHz after | 2 MHz before | 2 MHz after |
|---|--:|--:|--:|--:|
| `bios` | 8.54× | 8.93× | 5.80× | 6.00× |
| `wizardslab` | 8.74× | 8.86× | 5.88× | 5.92× |
| `vdp-layers` | 7.12× | 7.36× | 4.81× | 4.90× |
| `worst` | 4.25× | 4.27× | 3.46× | 3.47× |

The "after" build is not slower. The differences are within run-to-run noise.
A full bench of the "after" build clears every floor.

---

Video.test.ts against the core
------------------------------

```
PICOVDP_ADDON=…/6502-PICOVDP/host/node/Video.cjs npm run test:picovdp
Tests:       338 failed, 4 passed, 342 total
```

All 342 tests are enumerated against the adapter, and the same file passes
342/342 against `Video.ts`. The failures are the adapter's inspection methods
naming the phase that gives the core their accessors: `setVramByte` (213
tests), `getRegister` (20), `getStatus` (17), `getVramByte` (15), `getMode`
(14), `getDisplayLine` (11), and so on. Fourteen more are assertions on reads
and frames that the empty core gets wrong.

The 4 passes are vacuous against an empty core, which reads 0 and builds
index 0:

- the 320 × 240 RGBA buffer's size
- a black transparent backdrop
- `frameIndices`' length
- the collision bitmap reading zero

**The surface is wider than section 4 listed.** Besides the plan's list, the
tests call `readVRAM`, `writeVRAM` and `vramSize`, and three tests call
`serialize`/`deserialize`: two restore the raster from a snapshot, and one the
palette. Snapshots are the emulator's, not the card's. Phase 3 decides whether
the core exports its state or those three are skipped by name. No test reaches
into the class's internals, so `jest.picovdp.cjs` skips nothing.

---

The fuzzer
----------

`tools/fuzz.mjs` loads `Video.ts` and the adapter into one process. It feeds
both a seeded stream biased toward meaningful registers, addresses and values:
register writes, pointer commands with `VBANK`, data bursts, status selects and
reads, ticks, half-commands and resets. It compares:

- every read
- `/INT` after every operation and every tick
- the frame, every *N* operations

A divergence is minimised by deleting chunks while it persists. It is written to
`build/fuzz/` as the operation list, and as a TRACE.md trace recorded from
`Video.ts`.

| Run | Result |
|---|---|
| `--self --ops 2000000` | no divergence, 2.1 s |
| against the empty core, seed 7 | diverges at operation 93, "read `$00`, Video.ts read `$FB`"; minimised to 2 operations (`W 0 fb`, `R 0 fb` — a write loads the prefetch, §4), and the trace reads back through `tools/lib/trace.mjs` |
| against `Video.ts` with one frame pixel flipped after 300,000 ticks | caught at the next frame comparison |
| against `Video.ts` with `/INT` inverted for one tick | caught on that tick |

---

Done when
---------

| Check | Result |
|---|---|
| replaying each fixture's trace reproduces all fifteen checkpoints byte for byte — index frame, VRAM and JSON | ✅ `npm run replay:traces`, on the emulator's copies and on the pinned copies here, and in `Traces.test.ts` |
| with every recorded read matching | ✅ a read that differs is an event line that differs, and stops the replay |
| every checkpoint has a class and a settle point | ✅ all fifteen static; `sync-oracle.mjs` refuses a trace without them |
| `npm run test:picovdp` enumerates all 342 tests against the adapter, and they fail | ✅ 342 enumerated, 338 fail, 4 pass vacuously |
| attaching the observer moves no golden; `npm test` stays green | ✅ `capture:goldens -- --check`; 1,925 tests; `Traces.test.ts` checks it on every run |
| the host suite passes | ✅ `cmake --workflow --preset host`: `oracle_pinned`, `test_placeholder`, `spike_reference`, `node_addon_loads`; `host-asan`: the same without the addon |
| firmware presets still build | ✅ `pico2`, with `core/` compiled for the RP2350 and no warnings |

---

Differences from the plan
-------------------------

1. **The trace is gzip-compressed text, `<fixture>.vdpt.gz`, kept beside the
   goldens.** The plan named `<fixture>.vdpt` and left the encoding to
   TRACE.md.
2. **Line starts are not run-length encoded, and operations carry no display
   line or tick within the line.** Every line start is its own event with its
   tick delta. Display line and tick in line are derived (TRACE.md section 5).
   Run-length line starts would have lost the tick of each line start, which
   the tick-exact executors need. gzip gets the compression back.
3. **Static means "reproduces from the settle point", not "no operation in the
   window".** See [Checkpoints](#checkpoints). Section 4's definition would
   have made twelve checkpoints dynamic over status polls.
4. **Checkpoints carry `frame` and `window` as well as `class` and `settle`.**
   Injection needs the frame to place its snapshot marker.
5. **Recording annotates.** `record-traces.mjs` runs the replay's analysis and
   writes the class and settle point as it records. `replay-trace.mjs` checks
   them again, rather than writing them into the trace.
6. **The emulator gained two test files**, `VideoObserver.test.ts` and
   `Traces.test.ts`, and `runFixture` a `beforeReset` option.
7. **`PICOVDP_ADDON` names the adapter module** (`host/node/Video.cjs`), which
   finds the compiled addon itself, or through `PICOVDP_NODE_BINARY`.
8. **The adapter's surface is wider than section 4's list** (above). Phase 3
   decides the snapshot tests.
9. **The addon is built by CMake, not node-gyp, and not under `host-asan`.** An
   ASan-instrumented addon cannot load into an uninstrumented Node.
10. **`core/` exists from this phase**, as section 3's interface over empty
    bodies, and it is compiled for the RP2350 as well as the host.
11. **The emulator's commits were pushed**, at the owner's instruction in this
    session: these three, and the four from Phase 1 that were waiting. This
    repository has no remote, and rule 10 still applies to it.

---

Reproducing
-----------

```sh
# 6502-EMULATOR, on v3-vdp
npm run record:traces -- --check     # the traces are current
npm run replay:traces                # all fifteen checkpoints, exact
npm test                             # 1,925, Traces.test.ts included
PICOVDP_ADDON=$PWD/../../C/6502-PICOVDP/host/node/Video.cjs npm run test:picovdp

# here
cmake --workflow --preset host       # builds the addon; oracle_pinned, node_addon_loads
node tools/sync-oracle.mjs --check
node tools/fuzz.mjs --self --ops 2000000
node tools/fuzz.mjs --seed 7         # against the core: diverges, writes build/fuzz/
```
