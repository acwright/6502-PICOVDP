Oracle
======

**Written by `tools/sync-oracle.mjs`. Do not edit anything here by hand**
(PLAN.md ground rule 4). A golden that needs to move is re-captured in
6502-EMULATOR, in a commit of its own, and re-synced here in a commit of its own.

A pinned copy of 6502-EMULATOR's golden checkpoints and the traces of the
programs that produced them, from `v3-vdp` at
`527b53c81d4a0dbb848b807e802c58a6fae90dba` (2026-09-16T13:26:32-05:00).

- `<fixture>/<checkpoint>.idx.bin` — the frame as 76,800 palette indices, row-major. **The oracle**: compared exactly.
- `<fixture>/<checkpoint>.vram.bin` — all 64 KB of VRAM at the checkpoint.
- `<fixture>/<checkpoint>.json` — registers, mode, `STAT0`, VRAM hash, text grid.
- `<fixture>/<checkpoint>.png` — the frame in colour, to look at.
- `<fixture>/<fixture>.vdpt.gz` — the fixture's port traffic, in [docs/TRACE.md](../../docs/TRACE.md)'s format, version 1.
- `manifest.json` — the emulator commit, each checkpoint's class and settle point, and every file's SHA-256.

`node tools/sync-oracle.mjs --check` (CTest `oracle_pinned`) checks this directory against the manifest.

| Fixture | Checkpoint | Cycles | Frame | Settle | Window | Class |
|---|---|--:|--:|--:|--:|---|
| `bios` | `ok` | 7000000 | 419 | 4230 | 0 | static |
| `bios` | `screenful` | 10680000 | 639 | 4623 | 0 | static |
| `bios` | `scroll` | 14020000 | 840 | 12597 | 0 | static |
| `wizardslab` | `frame-60` | 1000000 | 59 | 90457 | 1576 | static |
| `wizardslab` | `frame-180` | 3000000 | 179 | 292524 | 1576 | static |
| `wizardslab` | `frame-300` | 5000000 | 299 | 494405 | 1576 | static |
| `wizardslab` | `frame-600` | 10000000 | 599 | 999182 | 1576 | static |
| `vdp-modes` | `text` | 600000 | 35 | 57637 | 1687 | static |
| `vdp-modes` | `compact` | 1650000 | 98 | 166649 | 1688 | static |
| `vdp-modes` | `graphics` | 2716667 | 162 | 277247 | 1690 | static |
| `vdp-modes` | `full` | 3783334 | 226 | 388461 | 1689 | static |
| `vdp-layers` | `parallax` | 1500000 | 89 | 120301 | 1689 | static |
| `vdp-layers` | `scroll-bit8-l1` | 3000000 | 179 | 285508 | 1689 | static |
| `vdp-layers` | `occluded` | 4000000 | 239 | 395655 | 1689 | static |
| `vdp-layers` | `scroll-bit8-l0` | 5000000 | 299 | 505793 | 1689 | static |
| `vdp-font` | `reset` | 550000 | 32 | 53590 | 1688 | static |
| `vdp-font` | `loaded` | 1616667 | 96 | 166566 | 1687 | static |
| `vdp-font` | `relocated` | 2683334 | 160 | 229219 | 0 | static |
