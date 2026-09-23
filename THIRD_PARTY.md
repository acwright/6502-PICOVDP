Third-Party Code
================

This repository is MIT-licensed (see `LICENSE`). It includes or links the
following third-party code, each under its own licence.

| Component | Where | Version | Licence |
|---|---|---|---|
| [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) | `external/pico-sdk` (submodule) | 2.1.1 | BSD-3-Clause |
| [TinyUSB](https://github.com/hathach/tinyusb) | `external/pico-sdk/lib/tinyusb` (SDK submodule) | 0.18.0 | MIT |
| [pico9918](https://github.com/visrealm/pico9918) board header `pico9918pro.h` (itself derived from the Pico SDK) | `firmware/boards/pico9918pro.h` | pico9918 `7d80992` (v1.2.0+16), unchanged | BSD-3-Clause |
| [pico9918](https://github.com/visrealm/pico9918) firmware files, below | `firmware/vga/`, `firmware/pio-utils/` | pico9918 `7d80992` (v1.2.0+16) | MIT |

Files taken from pico9918's firmware keep their original MIT headers and are
listed here as they arrive (PLAN.md, Appendix A):

| pico9918 | Here | Licence |
|---|---|---|
| `src/vga/vga.c`, `vga.h` (with `vga-modes.c`'s 640 × 480 timing) | `firmware/vga/vga.c`, `vga.h` — modified in Phase 8: VGA 640 × 480 only; a PIO interrupt at every screen line's start; the RGB buffer chosen per row at its line start; explicit priorities. The changes are listed at the top of `vga.c` | MIT |
| `src/vga/vga.pio` | `firmware/vga/vga.pio` — unchanged, Phase 8 | MIT |
| `src/pio-utils/` | `firmware/pio-utils/` — unchanged, Phase 8 | MIT |
| `src/tms9918.pio` | `firmware/bus.pio` — Phase 11: `tmsWrite` unchanged; `tmsRead` rewritten for four ports, from a word staging all four answers, with a jump table on MODE1:MODE. The bus handlers in `firmware/bus.c` are written anew around the core, after `src/main.c`'s | MIT |

pico9918 is © Troy Schrapel, MIT-licensed for its firmware
(`LICENSE_FIRMWARE.md` in that repository).
