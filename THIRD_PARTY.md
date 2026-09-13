Third-Party Code
================

This repository is MIT-licensed (see `LICENSE`). It includes or links the
following third-party code, each under its own licence.

| Component | Where | Version | Licence |
|---|---|---|---|
| [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) | `external/pico-sdk` (submodule) | 2.1.1 | BSD-3-Clause |
| [TinyUSB](https://github.com/hathach/tinyusb) | `external/pico-sdk/lib/tinyusb` (SDK submodule) | 0.18.0 | MIT |
| [pico9918](https://github.com/visrealm/pico9918) board header `pico9918pro.h` (itself derived from the Pico SDK) | `firmware/boards/pico9918pro.h` | pico9918 `7d80992` (v1.2.0+16), unchanged | BSD-3-Clause |

Files taken from pico9918's firmware keep their original MIT headers and are
listed here as they arrive (PLAN.md, Appendix A):

| pico9918 | Here | Licence |
|---|---|---|
| `src/vga/` | `firmware/vga/` | MIT — Phase 8 |
| `src/pio-utils/` | `firmware/pio-utils/` | MIT — Phase 8 |
| `src/tms9918.pio` | `firmware/bus.pio` | MIT — Phase 11 |

pico9918 is © Troy Schrapel, MIT-licensed for its firmware
(`LICENSE_FIRMWARE.md` in that repository).
