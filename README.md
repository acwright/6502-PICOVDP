6502-PICOVDP
============

Firmware for a custom Video Display Processor for the AC6502, running on
PICO9918 PRO v2.0 hardware (RP2354A).

- [SPEC.md](SPEC.md) — what the VDP does (draft 0.4). `6502-EMULATOR`'s
  `Video.ts` on its `v3-vdp` branch is the reference implementation.
- [PLAN.md](PLAN.md) — how the firmware is built, proven and delivered.
- [docs/TRACE.md](docs/TRACE.md) — the format of the port traces the goldens
  are replayed from.
- `docs/results/` — what each phase measured and checked.

Status: Phase 4 (the core's line timing, status registers and interrupts)
done. The bus side of the card — ports, registers, VRAM, palette, status and
`/INT` — is in C and matches `Video.ts` over 10⁷ fuzzed operations; lines build
as the backdrop until the tile engine arrives in Phase 5. See
`docs/results/phase-04.md`.

Layout
------

| Path | Holds |
|---|---|
| `core/` | portable C11: all of SPEC.md's behaviour, no hardware. `vdp.h` is the card; `vdp_debug.h` inspects it. The bus side (§3's line numbering, §4–§7, §11, §14, §15) is in; the tile engine and sprites follow |
| `host/node/` | the core as a Node-API addon, and `Video.cjs`, which presents it as the emulator's `Video` |
| `firmware/` | RP2350 only: main, bus PIO, VGA, debug link |
| `spike/` | Phase 1 timing spike: renderer, worst-case scenes, Pico 2 harness (disposable) |
| `tests/unit/` | C unit tests, run by CTest |
| `tests/oracle/` | the emulator's goldens and traces, pinned. Written by `tools/sync-oracle.mjs` only |
| `tools/` | Node ESM host tools (`sync-oracle`, `fuzz`; `vdpctl` to come) |
| `bench/nano/` | Arduino Nano bus harness (PlatformIO) |
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
node tools/sync-oracle.mjs            # re-sync; the emulator must be on v3-vdp, clean
```

A re-sync is a commit of its own (PLAN.md ground rule 4).

### The core against the emulator

Build the `host` preset first. Then, in `6502-EMULATOR`:

```sh
PICOVDP_ADDON=/path/to/6502-PICOVDP/host/node/Video.cjs npm run test:picovdp
```

That runs the emulator's `Video.test.ts`, unchanged, against the core. Here:

```sh
node tools/fuzz.mjs --scope status --ops 10000000  # Video.ts against the core, over what the core has so far
node tools/fuzz.mjs --seed 1 --ops 100000          # everything: reads, /INT and frames (diverges until Phase 7)
node tools/fuzz.mjs --self --scope status          # Video.ts against itself: the harness's own check
```

The fuzzer loads the emulator's compiled `Video`, so run its `npm run build:cli`
first. A divergence is minimised and written to `build/fuzz/` as an operation
list and, when the operations fit a trace, as a trace.

### Firmware

| Preset | Board | Build |
|---|---|---|
| `pico2` | Raspberry Pi Pico 2 | debug: USB CDC log, picotool reset |
| `pro-debug` | PICO9918 PRO v2.0 | debug |
| `pro-release` | PICO9918 PRO v2.0 | release: no USB |

```sh
cmake --preset pico2 && cmake --build --preset pico2
picotool load -x -f build/pico2/firmware/picovdp.uf2
```

The first flash needs the board in BOOTSEL (hold BOOT while plugging in USB).
After that a debug build can be reflashed while it runs: `-f` asks it to reboot
into BOOTSEL over USB. Its log appears on `/dev/cu.usbmodem*`.

### Nano harness

```sh
cd bench/nano && pio run              # pio run -t upload to flash
```

Licence
-------

MIT — see [LICENSE](LICENSE). Third-party code is listed in
[THIRD_PARTY.md](THIRD_PARTY.md).
