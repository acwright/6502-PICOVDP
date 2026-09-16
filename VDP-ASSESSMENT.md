# VDP assessment: 6502-PICOVDP

> An outline, not a plan. The detailed plan for this repository's part in the rollout goes
> in `VDP-PLAN.md`, written in a session of its own. This repository's own firmware plan
> stays in `PLAN.md`. Surveyed 2026-09-16 across the whole workspace.

## The change

The ACE moves from a Pico9918 running stock TMS9918A firmware to the **6502-PICOVDP**
(this repository's `SPEC.md`) on PICO9918 PRO v2.0 hardware, running **BIOS 2.x**. Everything
else stays where it is: COB, DEV, KIM, VCS, PicoCalc, and any ACE whose card cannot be
reflashed (RP2040 pico9918 v1.0–1.3). Those keep the stock firmware and **BIOS 1.x**,
whose last release is **1.6**.

- **Legacy** in these documents means TMS9918A + BIOS 1.x. **VDP** means PICOVDP + BIOS 2.x.
- **Compatibility runs one way.** The PICOVDP's legacy submode runs Text and Graphics I
  programs unchanged, so BIOS 1.x and existing cartridges run on it. Graphics II and
  Multicolor fall back to Graphics I and draw garbage. Register writes above 7 no longer
  alias, so F18A tricks break. Sprites per line are 16 by default, not 4. Nothing written
  for the VDP runs on a TMS9918A.

## Decisions already made

- **No new repositories.**
- **BIOS 1.6 is the last 1.x release.** It is 1.5 plus the NVRAM save slots in
  `6502-BIOS/PLAN.md`, and nothing else. It ships in emulator **2.7.0**, and the frozen
  legacy docs document it.
- **BIOS 2.0 is 1.6 plus:**
  - The PICOVDP work in `6502-EMULATOR`'s `docs/handoff/6502-BIOS.md` (branch `v3-vdp`):
    card detection, hardware scroll, port B for interrupt handlers, `WaitVBlank`.
  - A console in the PICOVDP's **Text mode** (`VMODE $1`, 40×24, 6×8 cells) with a
    **per-cell colour table**. It keeps the same font and every screen layout.
  - **No Monitor.** The machine **boots straight to BASIC**, with a new header and a colour
    logo drawn from the font's CP437 block characters. Wozmon stays at `$FF00`.
  - **The font lives in the PICOVDP firmware.** The card loads it into VRAM at reset and
    on command (a new register and a capability bit, SPEC draft 0.5). ROM `$B800` holds
    no font on 2.x.
  - **ROM layout:** BASIC takes the Monitor's 4.3 KB (`$C000–$FEFF`), and the Kernal takes
    all of `$A000–$BFFF`, including the space the font used. Nothing the Kernal needs goes
    above `$C000`, because cartridges overlay `$C000–$FFFF`. The Kernal holds the
    primitives cartridges need; BASIC-only work lives in BASIC.
  - **No TMS9918A support.** BIOS 2.x runs only with a PICOVDP, with no fallback paths.
  - **New BASIC commands with matching Kernal entries.**
    - Core: `SCREEN`, `VPOKE`/`VPEEK`, `VREG`, `PALETTE`, `VSYNC`, `VLOAD`.
    - Second tier, if room is found: `SPRITE`, `SCROLL`, `LAYER`, `VSTAT`.
    - Save-slot commands, if room is found.
    - `SYS addr[,a,x,y]`, and `BLOAD`/`BSAVE` over XModem when given no filename.
    - BASIC returns to the text console when a program stops.
  - **Tokens:** every 1.x token keeps its value, and new keywords are appended after `$D4`.
    The `BRK` statement is retired and its token `$B4` goes to a new keyword.
  - **A BRK instruction** prints `BREAK $nn AT $xxxx  A= X= Y= P= S=` and warm-starts
    BASIC. `BRK_PTR` stays hookable.
  - **`COLOR fg[,bg[,border]]`** sets the pen for later output, `CLS` fills the screen with
    it, and `border` is register 7's low nibble.
  - **Existing jump-table addresses do not move.** New entries are appended.
- **6502-EMULATOR** makes the video card an option (TMS9918A or PICOVDP): one app, one
  site. It also publishes a frozen **2.7.0** web build at `/6502-EMULATOR/v2/` for the
  legacy docs.
- **6502-DOCS** is versioned: legacy docs (BIOS 1.6) are frozen at `/6502-DOCS/v1/`, and
  the main site is rewritten for the VDP and BIOS 2.x.
- **6502-BIOS** gets a `v1.x` branch cut at `v1.6`; `main` becomes 2.x.
- **Assembly and C projects** get a VDP include chosen by a build option, not branches.
  The legacy `6502.inc` gets one last update, for 1.6.
- **EhBASIC and vc83basic** stay 1.x. **PicoCalc** and **KIMULATOR** stay legacy and ship BIOS 1.6. **The YouTube series**
  teaches the legacy VDP and mentions the new features.

## Order across the workspace

**Part 1: BIOS 1.6, the last legacy release**

1. **6502-BIOS:** build 1.6 on `main`, tag `v1.6`, and cut `v1.x` from it.
2. **6502-EMULATOR `main`:** bundle 1.6, release **2.7.0**, and publish its frozen web build
   at `/6502-EMULATOR/v2/`. Then merge `main` into `v3-vdp` and re-capture the goldens
   there; they exist only on that branch.
3. **6502-PICOVDP:** re-sync `tests/oracle/`, whose pinned `bios` goldens moved.
4. **The legacy include** gains the NVRAM entries in every copy: 6502-ASM, 6502-CRT,
   6502-PRG, 6502-BIN, 6502-EHBASIC, 6502-C (with `6502.h`) and WIZARDSLAB.
5. **6502-DOCS `main`** documents 1.6 and pins 2.7.0. Then it cuts `v1`, published at
   `/6502-DOCS/v1/`, against the emulator's frozen 2.7.0 build at `/6502-EMULATOR/v2/`.

Alongside steps 2–5, once step 1 is tagged: **6502-PICOCALC** embeds the `v1.6` ROM and
releases a new UF2, and **6502-KIMULATOR** bundles it and releases 1.0.9. DOCS waits for both
releases before cutting `v1`.

**Part 2: the VDP**

6. **6502-PICOVDP:**
   - SPEC draft 0.5 adds the built-in font and its load command. The emulator's PICOVDP
     card implements it first, then the firmware.
   - Firmware proven on the PRO (its Phases 9–11) gates the hardware switch, not the
     software work.
7. **6502-EMULATOR:** `v3-vdp` merged, with the card as an option; tagged 3.x.
8. **6502-BIOS:** 2.0 on `main`. This can start once step 1 is done, because the `v3-vdp`
   emulator already runs the PICOVDP. Its console work needs the built-in font in the
   emulator (step 6).
9. **6502-ASM** sets the VDP include convention. 6502-CRT, 6502-PRG, 6502-BIN and 6502-C
   follow it.
10. **Everything else follows BIOS 2.0:**
    - The emulator bundles BIOS 2.0.
    - 6502-DOCS `main` is rewritten.
    - bastok gains the 2.x token table.
    - 6502-ACE, WIZARDSLAB, 6502-EHBASIC, vc83basic, cffs and 6502-ASSEMBLY follow.

---

## This repository's role

The specification and the firmware. It is the **hardware gate** for the whole rollout:
BIOS 2.0 and the rewritten docs should not be published as "the ACE" until the firmware
runs on the PRO's bus.

## Where it stands

- Phase 8 is done: everything but the bus runs on a Pico 2.
- Phases 9–11 (the bus) wait for the PRO.
- `SPEC.md` is at draft 0.4. `6502-EMULATOR`'s `Video.ts` on `v3-vdp` is the reference
  implementation, and `tests/oracle/` is pinned from it.

## Work outline

1. **Finish Phases 9–11** per `PLAN.md`. Nothing in the rollout changes them.
2. **The built-in font (SPEC draft 0.5).** BIOS 2.0's console depends on it.
   - **Reset:** after the default palette, the firmware writes the font into VRAM where
     the reset-state text layout expects its pattern table. Update §7 and §15.
   - **Load command:** a reserved register (`$28`–`$7F`) that copies a font into the
     current layer-0 pattern table (`L0PAT`), with room for a font ID.
     - ID 0 is the 6×8 CP437 set, taken byte-for-byte from `6502-BIOS/Chars.asm`.
     - A later ID could be a true 8×8 set for a 40×30 Full-mode console.
   - **Timing:** a 2 KB copy doesn't fit in the per-access service budget (§4, about 700
     RP2350 cycles). The copy runs outside the access path, and the SPEC states when it
     is complete (for example, by the next vertical blank) and what reads see before then.
   - **Capability:** a `STAT6` bit for the built-in font (b7:6 are reserved; b6 is
     earmarked for a blitter). Update §16 and §17.
   - **One source of truth:** the font lives here as data, not as a copy typed into C,
     and the emulator's PICOVDP card is generated or synced from the same file, as its
     default palette mirrors `vdp_default_palette`. The oracle and goldens must agree.
   - **Order:** the spec decides first, then the emulator's `Video.ts` implements it (the
     reference), then `core/` and the firmware.
3. **Follow the emulator's merge.** When `v3-vdp` merges and the card becomes an option,
   these break:
   - `tools/sync-oracle.mjs` (via `tools/lib/emulator.mjs` `EMULATOR_BRANCH`) refuses any
     branch but `v3-vdp`. Move it to `main` or to a tag.
   - `host/node/Video.cjs` is presented as the emulator's `src/core/IO/Video`, mapped by
     the emulator's `jest.picovdp.cjs`. If the emulator renames the PICOVDP card, update
     the adapter, and the references in `README.md`, `docs/TRACE.md` and the tools.
   - Re-sync `tests/oracle/` from the merged commit. The merge itself should not move
     the goldens, and re-syncing proves it.
4. **Re-sync when the BIOS changes.** The `bios` goldens pinned here are captured from the
   emulator's bundled ROM, so they move twice:
   - BIOS 1.6: the splash text, when emulator 2.7.0 is merged into `v3-vdp`.
   - BIOS 2.0: the whole console, in Text mode with per-cell colour, a new header and a
     hardware scroll.
   Each is a re-sync in a commit of its own, and the firmware must still reproduce every
   checkpoint.
5. **One spec.** `SPEC.md` here duplicates `6502-EMULATOR/docs/VDP-SPEC.md`, and
   `docs/SPEC.html` is rendered from it. Agree which is the source and how the copy is
   kept in step.
6. **The detection contract.**
   - BIOS 2.0 identifies this card through §16 (`STAT4` → `$AC`), checks the firmware
     version (`STAT5`) and the font bit (`STAT6`), and doesn't run on a TMS9918A at all.
   - After that, those values are ABI. Changing them is a BIOS change too.
7. **A release people can install.** A versioned UF2 on GitHub releases, with flashing
   instructions. 6502-ACE's README and 6502-DOCS's getting-started pages link to it, and
   both need the "RP2040 boards (v1.0–1.3) cannot run this" note (§2).
8. **Optional bench acceptance:** BIOS 2.0 booting and scrolling on the PRO. It is the
   first real software to use `VMODE $1` with a per-cell attribute table and `L0SCRY` on
   silicon.

## Linked repositories

| Repository | Path | Why |
|---|---|---|
| 6502-EMULATOR | `~/Developer/NodeJS/6502-EMULATOR` | The oracle and the reference implementation; branch name and file path coupling |
| 6502-BIOS | `~/Developer/Assembly/6502-BIOS` | §16 detection and §17's Kernal changes become BIOS 2.0; `Chars.asm` is the font's origin, and 2.0 relies on the load command |
| 6502-DOCS | `~/Developer/NodeJS/6502-DOCS` | New-mode chapters are written from `SPEC.md`; flashing instructions |
| 6502-ACE | `~/Developer/Kicad/6502-ACE` | Hardware requirement and firmware link |
| 6502-ASM (then CRT, PRG, BIN, C) | `~/Developer/Assembly/6502-ASM` | The VDP include names §5's registers and §4's ports |

## Questions for VDP-PLAN.md

1. The canonical spec location and the sync direction.
2. The firmware version scheme versus spec drafts, and what version BIOS 2.0 requires.
3. Whether the oracle pins a tag instead of a branch after the merge.
