6502-PICOVDP
============

Firmware for a custom Video Display Processor for the AC6502, running on
PICO9918 PRO v2.0 hardware (RP2354A).

- [SPEC.md](SPEC.md) — what the VDP does (draft 0.3). `6502-EMULATOR`'s
  `Video.ts` on its `v3-vdp` branch is the reference implementation.
- [PLAN.md](PLAN.md) — how the firmware is built, proven and delivered.
- `docs/results/` — what each phase measured and checked.

Status: Phase 0 (repository and toolchain).

Layout
------

| Path | Holds |
|---|---|
| `core/` | portable C11: all of SPEC.md's behaviour, no hardware (from Phase 3) |
| `firmware/` | RP2350 only: main, bus PIO, VGA, debug link |
| `tests/unit/` | C unit tests, run by CTest |
| `tools/` | Node ESM host tools (`vdpctl`, `fuzz`, `sync-oracle`) |
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
