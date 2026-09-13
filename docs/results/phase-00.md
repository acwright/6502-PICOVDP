Phase 0 — Repository and toolchain
==================================

**Status:** done. Every check below passed on 2026-09-13.

Toolchain
---------

| Tool | Version | Notes |
|---|---|---|
| Arm GNU Toolchain | 15.3.Rel1 (GCC 15.3.1) | `/Applications/ArmGNUToolchain/15.3.rel1`. It had already been reinstalled, so the reinstall step was not needed. newlib links. |
| pico-sdk | 2.1.1 | `external/pico-sdk` |
| TinyUSB | 0.18.0 (`86ad6e5`) | the only SDK submodule initialised |
| picotool | 2.3.0 | Homebrew; the SDK finds it through `/opt/homebrew/lib/cmake/picotool` and doesn't build its own |
| CMake | 4.4.3 | |
| Ninja | Homebrew | |
| Node | 26.4.0 | |
| PlatformIO Core | 6.1.19 | `~/.platformio/penv/bin/pio`, **not on PATH**; atmelavr 5.0.0 |
| ffmpeg | Homebrew | |

Decisions
---------

- **C dialect.** The host builds strict C11 (`-std=c11`). RP2350 builds use
  `gnu11`, because the SDK's sources are compiled into the firmware target and
  use `typeof`.
- **Warnings.** `-Wall -Wextra` applies to this repo's targets on every
  platform, through the `picovdp_warnings` interface library. `-Werror` is added
  on the host only. The SDK is not held to these flags.
- **`PICO_PLATFORM`** is `rp2350-arm-s` for all three firmware presets.
- **Pico 2 board file.** Phase 0 uses the SDK's own `pico2.h`.
  `firmware/boards/` so far holds only `pico9918pro.h`, copied unchanged.
- **`tools/package.json`** has no dependencies yet. `serialport` is added with
  the first tool that talks to a board.

Done when
---------

| Check | Result |
|---|---|
| `cmake --preset host && ctest --preset host` passes a placeholder test | ✅ `test_placeholder` passes; `cmake --workflow --preset host` runs configure, build and test |
| `host-asan` builds and passes | ✅ |
| `pico2` preset builds a UF2 | ✅ `build/pico2/firmware/picovdp.uf2`, no warnings |
| `pro-debug`, `pro-release` build | ✅ both use `firmware/boards/pico9918pro.h` |
| `bench/mega` builds | ✅ no warnings in this repo's code |
| the `pico2` UF2 logs over USB CDC | ✅ one line a second on `/dev/cu.usbmodem11101` |
| a second `picotool load -x -f` reflashes the running Pico 2 with no button | ✅ `picotool load -x -v -f`: it asked the board to reboot into BOOTSEL, the verify passed, the board restarted and uptime began again from 0 |

The Pico 2
----------

RP2350 **A2**, QFN60, 4 MB flash, chip id `B8958606A8F33A40`. The clock is at
the SDK default of 150 MHz. Phase 1 sets the real presets.

```
picovdp uncommitted debug board pico2 id B8958606A8F33A40 sys 150000000 Hz uptime 1003 ms tick 1
```

The board came from another project. Its flash was saved before the first
flash, outside this repository.
