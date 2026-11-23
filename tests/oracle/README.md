Oracle
======

**Written by `tools/sync-oracle.mjs`. Do not edit anything here by hand**
(DESIGN.md ground rule 4). A golden that needs to move is re-captured in
6502-EMULATOR, in a commit of its own, and re-synced here in a commit of its own.

A pinned copy of 6502-EMULATOR's golden checkpoints and the traces of the
programs that produced them, from `main` at
`468f5391c791f1304a0c83ea4cd8ecd743d8686e`, `v3.7.0` (2026-11-23T06:21:20-06:00).

- `<fixture>/<checkpoint>.idx.bin` — the frame as 76,800 palette indices, row-major. **The oracle**: compared exactly.
- `<fixture>/<checkpoint>.vram.bin` — all 64 KB of VRAM at the checkpoint.
- `<fixture>/<checkpoint>.json` — registers, mode, `STAT0`, VRAM hash, text grid.
- `<fixture>/<checkpoint>.png` — the frame in colour, to look at.
- `<fixture>/<fixture>.vdpt.gz` — the fixture's port traffic, in [docs/TRACE.md](../../docs/TRACE.md)'s format, version 1.
- `manifest.json` — the emulator commit, each checkpoint's class and settle point, and every file's SHA-256.

`node tools/sync-oracle.mjs --check` (CTest `oracle_pinned`) checks this directory against the manifest.

| Fixture | Checkpoint | Cycles | Frame | Settle | Window | Class |
|---|---|--:|--:|--:|--:|---|
| `bios` | `ok` | 1000000 | 59 | 4154 | 0 | static |
| `bios` | `screenful` | 4660000 | 278 | 4682 | 0 | static |
| `bios` | `scroll` | 8000000 | 479 | 5182 | 0 | static |
| `wizardslab` | `frame-60` | 1000000 | 59 | 91658 | 1535 | static |
| `wizardslab` | `frame-180` | 3000000 | 179 | 295725 | 1535 | static |
| `wizardslab` | `frame-300` | 5000000 | 299 | 499607 | 1534 | static |
| `wizardslab` | `frame-600` | 10000000 | 599 | 1009383 | 1593 | static |
| `vdp-modes` | `text` | 600000 | 35 | 59271 | 1687 | static |
| `vdp-modes` | `compact` | 1650000 | 98 | 168411 | 1687 | static |
| `vdp-modes` | `graphics` | 2716667 | 162 | 279009 | 1689 | static |
| `vdp-modes` | `full` | 3783334 | 226 | 390222 | 1690 | static |
| `vdp-layers` | `parallax` | 1500000 | 89 | 120778 | 1689 | static |
| `vdp-layers` | `scroll-bit8-l1` | 3000000 | 179 | 285986 | 1689 | static |
| `vdp-layers` | `occluded` | 4000000 | 239 | 396134 | 1689 | static |
| `vdp-layers` | `scroll-bit8-l0` | 5000000 | 299 | 506271 | 1689 | static |
| `flash-cart` | `three-banks` | 500000 | 29 | 52838 | 1687 | static |
| `flash-cart` | `self-programmed` | 1500000 | 89 | 109967 | 0 | static |
| `vdp-font` | `reset` | 550000 | 32 | 53590 | 1688 | static |
| `vdp-font` | `loaded` | 1616667 | 96 | 166566 | 1687 | static |
| `vdp-font` | `relocated` | 2683334 | 160 | 229219 | 0 | static |
