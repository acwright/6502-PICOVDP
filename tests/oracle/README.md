Oracle
======

**Written by `tools/sync-oracle.mjs`. Do not edit anything here by hand**
(PLAN.md ground rule 4). A golden that needs to move is re-captured in
6502-EMULATOR, in a commit of its own, and re-synced here in a commit of its own.

A pinned copy of 6502-EMULATOR's golden checkpoints and the traces of the
programs that produced them, from `main` at
`c0f76f24818632c081686b26734b0ba9b3907a6a`, `v3.2.0` (2026-09-17T19:38:48-05:00).

- `<fixture>/<checkpoint>.idx.bin` — the frame as 76,800 palette indices, row-major. **The oracle**: compared exactly.
- `<fixture>/<checkpoint>.vram.bin` — all 64 KB of VRAM at the checkpoint.
- `<fixture>/<checkpoint>.json` — registers, mode, `STAT0`, VRAM hash, text grid.
- `<fixture>/<checkpoint>.png` — the frame in colour, to look at.
- `<fixture>/<fixture>.vdpt.gz` — the fixture's port traffic, in [docs/TRACE.md](../../docs/TRACE.md)'s format, version 1.
- `manifest.json` — the emulator commit, each checkpoint's class and settle point, and every file's SHA-256.

`node tools/sync-oracle.mjs --check` (CTest `oracle_pinned`) checks this directory against the manifest.

| Fixture | Checkpoint | Cycles | Frame | Settle | Window | Class |
|---|---|--:|--:|--:|--:|---|
| `bios` | `ok` | 1000000 | 59 | 5027 | 0 | static |
| `bios` | `screenful` | 4660000 | 278 | 5555 | 0 | static |
| `bios` | `scroll` | 8000000 | 479 | 6055 | 0 | static |
| `wizardslab` | `frame-60` | 1000000 | 59 | 91598 | 1535 | static |
| `wizardslab` | `frame-180` | 3000000 | 179 | 293665 | 1535 | static |
| `wizardslab` | `frame-300` | 5000000 | 299 | 495546 | 1535 | static |
| `wizardslab` | `frame-600` | 10000000 | 599 | 1000323 | 1576 | static |
| `vdp-modes` | `text` | 600000 | 35 | 60144 | 1687 | static |
| `vdp-modes` | `compact` | 1650000 | 98 | 169284 | 1687 | static |
| `vdp-modes` | `graphics` | 2716667 | 162 | 279882 | 1689 | static |
| `vdp-modes` | `full` | 3783334 | 226 | 391095 | 1690 | static |
| `vdp-layers` | `parallax` | 1500000 | 89 | 121635 | 1689 | static |
| `vdp-layers` | `scroll-bit8-l1` | 3000000 | 179 | 286842 | 1689 | static |
| `vdp-layers` | `occluded` | 4000000 | 239 | 396991 | 1689 | static |
| `vdp-layers` | `scroll-bit8-l0` | 5000000 | 299 | 507127 | 1689 | static |
| `vdp-font` | `reset` | 550000 | 32 | 53590 | 1688 | static |
| `vdp-font` | `loaded` | 1616667 | 96 | 166566 | 1687 | static |
| `vdp-font` | `relocated` | 2683334 | 160 | 229219 | 0 | static |
