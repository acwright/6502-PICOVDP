6502-PICOVDP
============

Firmware for a custom Video Display Processor for the AC6502, running on
PICO9918 PRO v2.0 hardware (RP2354A: an RP2350A with 2 MB of flash in the
package).

- [SPEC.md](SPEC.md) — what the VDP does (draft 0.5). `6502-EMULATOR`'s
  `Video.ts`, released in 3.0.0, is the reference implementation.
  [docs/SPEC.html](docs/SPEC.html) is the same document, rendered, and is kept in
  step with it. SPEC.md is canonical; the emulator's `docs/VDP-SPEC.md` is a
  byte-identical copy, checked by `node tools/check-spec.mjs` (CTest
  `spec_in_step`, registered when the emulator checkout is found).
- [PLAN.md](PLAN.md) — how the firmware is built, proven and delivered.
- [docs/TRACE.md](docs/TRACE.md) — the format of the port traces the goldens
  are replayed from.
- [docs/DEBUGLINK.md](docs/DEBUGLINK.md) — the firmware's debug link over USB,
  and `vdpctl`.
- `docs/results/` — what each phase measured and checked.

Status: Phase 12 (traces through the bus) done. Every golden checkpoint the
emulator holds, all eighteen, reproduces exactly with its fixture's trace
played through the PRO's pins by the Nano, untimed, at each of three timing
profiles: each index frame from its settle point, and VRAM and registers at the
checkpoint. See `docs/results/phase-12.md`. Phase 11 (the bus) before it: all
four ports work through the PRO's pins,
driven by an Arduino Nano: 10⁷ random accesses at each of three timing profiles
with no read wrong against the emulator's `Video.ts`, back-to-back reads 4 µs
apart always right, RST performing §15, and no FIFO overrun. See
`docs/results/phase-11.md`. Before it, Phase 10 put everything but the bus on
the card itself: all eighteen golden checkpoints reproduce exactly on
the PRO by injection, and each one's picture is captured off the monitor and
scored against its golden; Phase 1's 36 worst-case scenes run with no late line
while snapshots stream; a deliberately late line shows the line before it and
leaves status untouched; and a fault or hang is recorded, recovered from and
reflashed over USB with no hands on the board. The PRO's RP2354A matches the
Pico 2 to 0.4% of a line. See `docs/results/phase-10.md`, and
`docs/results/phase-09.md` for the bench itself.

Layout
------

| Path | Holds |
|---|---|
| `core/` | portable C11: all of SPEC.md's behaviour, no hardware. `vdp.h` is the card; `vdp_debug.h` inspects it |
| `host/node/` | the core as a Node-API addon, and `Video.cjs`, which presents it as the emulator's `Video` |
| `host/replay/` | `vdp-replay`: a trace into the core in pure C, each checkpoint against its golden |
| `host/scene/` | `vdp-scene`: the firmware's worst-case scenes drawn on the host, the reference for the board's snapshots |
| `firmware/` | RP2350 only: `main.c`; `renderer.c`, the card on two cores; `vga/`, pico9918's VGA driver cut to §3's raster; `bus.pio` and `bus.c`, the four ports on the pins (Phase 11); `fault.c`, fault records, the watchdog and safe mode; and in debug builds `link.c`, `inject.c`, `scenes.c`, `profile.c` |
| `spike/` | Phase 1 timing spike: renderer, worst-case scenes, Pico 2 harness (disposable) |
| `fonts/` | `cp437-6x8.bin`, the card's built-in font (§7), from 6502-BIOS v1.6. Checked by `tools/font.mjs` |
| `tests/unit/` | C unit tests, run by CTest |
| `tests/bench/` | port conformance scripts for the Nano, played by `vdpctl conformance` |
| `tests/oracle/` | the emulator's goldens and traces, pinned. Written by `tools/sync-oracle.mjs` only |
| `tools/` | Node ESM host tools: `vdpctl`, `sync-oracle`, `replay`, `card`, `fuzz`, `font`, `check-spec` |
| `bench/nano/` | Arduino Nano bus harness (PlatformIO) |
| `bench/hardware/` | the bench's schematic |
| `bench/cards/` | the bench cards: pictures this repo draws itself, for the capture card to judge. Written by `tools/card.mjs` |
| `external/pico-sdk` | pico-sdk 2.1.1, submodule |

Getting started
---------------

Requirements on macOS: Arm GNU Toolchain (`brew install --cask
gcc-arm-embedded`), CMake ≥ 3.25, Ninja, `picotool` 2.x, Node ≥ 22, and
PlatformIO for the Nano harness.

```sh
git clone --recursive <this repo>        # or, in an existing clone:
git submodule update --init
git -C external/pico-sdk submodule update --init lib/tinyusb
```

Only TinyUSB is needed from the SDK's submodules; don't initialise the rest
recursively.

### Host: core and tests

```sh
cmake --workflow --preset host        # configure, build, ctest
cmake --workflow --preset host-asan   # the same under ASan and UBSan
```

The `host` preset also builds the Node-API addon,
`build/host/host/node/picovdp.node`, against the headers of the Node on `PATH`.
`host-asan` does not build it: an instrumented addon cannot load into Node.

### The oracle

The tools find `6502-EMULATOR` at `../../NodeJS/6502-EMULATOR`, or wherever
`PICOVDP_EMULATOR` points.

```sh
node tools/sync-oracle.mjs --check    # tests/oracle matches its manifest (CTest oracle_pinned)
node tools/sync-oracle.mjs            # re-sync; the emulator's HEAD must be on origin/main, clean
```

A re-sync is a commit of its own (PLAN.md ground rule 4).

### The font

```sh
node tools/font.mjs --check           # fonts/cp437-6x8.bin is font $00 (CTest font_pinned)
node tools/font.mjs show '$41-$5A'    # glyphs as 6 × 8 text
```

With a 6502-BIOS checkout at `../../Assembly/6502-BIOS` (or `PICOVDP_BIOS`), the
check also proves the file still derives from `v1.6:Chars.asm`.

### The core against the emulator

Build the `host` preset first. Then, in `6502-EMULATOR`:

```sh
PICOVDP_ADDON=/path/to/6502-PICOVDP/host/node/Video.cjs npm run test:picovdp
```

That runs the emulator's `Video.test.ts`, unchanged, against the core. Here:

```sh
node tools/replay.mjs bios --classes               # a fixture's trace into the core, against tests/oracle (CTest replay_<fixture>)
node tools/replay.mjs --reference                  # every fixture into Video.ts instead: the harness's own check
node tools/fuzz.mjs --scope tiles --ops 10000000   # Video.ts against the core: reads, /INT, status and every frame
node tools/fuzz.mjs --scope frames --ops 8000000   # the same over random layer and sprite scenes, ~27,000 frames
node tools/fuzz.mjs --seed 1 --ops 100000          # the all scope: every read, /INT, and a frame every 5,000 operations
node tools/fuzz.mjs --self --scope tiles           # Video.ts against itself: the harness's own check
```

And with no Node at all, after building the `host` preset (CTest
`vdp_replay_<fixture>`, also under `host-asan`):

```sh
build/host/host/replay/vdp-replay --classes tests/oracle/*/*.vdpt.gz
```

With `PICOVDP_SPLIT_CHECK=1` set, the adapter builds every row a second time
with the two cores' division at another column and throws if the row or the
status differs; any of the commands above can run that way.

The fuzzer and `replay.mjs --reference` load the emulator's compiled `Video`,
so run its `npm run build:cli` first. A divergence is minimised and written to
`build/fuzz/` as an operation list and, when the operations fit a trace, as a
trace.

### Firmware

| Preset | Board | Build |
|---|---|---|
| `pico2` | Raspberry Pi Pico 2 | debug: the debug link over USB, statistics, snapshots, injection |
| `pro-debug` | PICO9918 PRO v2.0 | debug |
| `pro-release` | PICO9918 PRO v2.0 | release: no USB, watchdog |

```sh
cmake --preset pico2 && cmake --build --preset pico2
node tools/vdpctl.mjs flash                       # picotool load -x -f, then INFO

cmake --preset pro-debug && cmake --build --preset pro-debug
PICOVDP_PRESET=pro-debug node tools/vdpctl.mjs flash
```

The first flash of a board needs it in BOOTSEL (hold BOOT while plugging in
USB). After that a debug build is reflashed while it runs. `PICOVDP_PRESET`
says which build `flash` sends and which ELF a fault record is symbolised
against; it defaults to `pico2`. The Pico 2 drives VGA on GPIO 0–13 with
nothing attached; on the PRO the same pins reach the DAC.

Driving it (all of `docs/DEBUGLINK.md` section 8):

```sh
node tools/vdpctl.mjs info                         # build, clock, reset reason, the last fault record
node tools/vdpctl.mjs stats                        # late rows, latency, stage maxima
node tools/vdpctl.mjs inject all                   # every golden checkpoint replayed on the board
node tools/vdpctl.mjs scenes --seconds 10 --stream # the worst-case scenes, snapshots checked
node tools/vdpctl.mjs late                         # a late line on purpose, checked against the host
node tools/vdpctl.mjs fault core1                  # a HardFault; the record, from safe mode
```

`scenes` and `late` need the `host` preset built, for `vdp-scene`.

With the capture card of `docs/BENCH.md` attached, the picture is scored too:

```sh
node tools/vdpctl.mjs inject all --capture         # each checkpoint's picture, against its golden
node tools/vdpctl.mjs card dac                     # each channel's sixteen levels, measured
node tools/vdpctl.mjs card palette                 # §11's 256 entries, to look at
node tools/card.mjs all --png                      # redraw the bench cards from the reference
node tools/card.mjs --check                        # the cards still replay to their goldens (CTest cards_pinned)
```

### Nano harness

```sh
cd bench/nano && pio run              # pio run -t upload to flash
```

With the Nano wired to the PRO (`docs/BENCH.md`), and the board on `pro-debug`:

```sh
node tools/vdpctl.mjs bus                          # the wiring check
node tools/vdpctl.mjs conformance --timing all     # all four ports, against Video.ts
node tools/vdpctl.mjs replay all --timing all      # every golden checkpoint through the pins
```

Licence
-------

MIT — see [LICENSE](LICENSE). Third-party code is listed in
[THIRD_PARTY.md](THIRD_PARTY.md).
