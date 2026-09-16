# VDP assessment: 6502-PICOVDP

> An outline, not a plan. The detailed plan for this repository's part in the rollout goes
> in `VDP-PLAN.md`, written in a session of its own. This repository's own firmware plan
> stays in `PLAN.md`. Surveyed 2026-09-16 across the whole workspace.

## The change

The ACE moves from a Pico9918 running stock TMS9918A firmware to the **6502-PICOVDP**
(this repository's `SPEC.md`) on PICO9918 PRO v2.0 hardware, running **BIOS 2.x**.
Everything else stays where it is: COB, DEV, KIM, VCS, PicoCalc, and any ACE whose card
cannot be reflashed (RP2040 pico9918 v1.0–1.3). Those keep the stock firmware and
**BIOS 1.x (1.5)**.

- **Legacy** in these documents means TMS9918A + BIOS 1.x. **VDP** means PICOVDP + BIOS 2.x.
- **Compatibility runs one way.** The legacy submode runs Text and Graphics I programs
  unchanged, so BIOS 1.5 and existing cartridges run here. Graphics II and Multicolor
  fall back to Graphics I and draw garbage. Register writes above 7 no longer alias, so
  F18A tricks break. Sprites per line are 16 by default, not 4. Nothing written for the
  VDP runs on a TMS9918A.
- **BIOS 2.0 is assumed to be:** BIOS 1.5, plus the NVRAM save slots in
  `6502-BIOS/PLAN.md`, plus the VDP work in `6502-EMULATOR`'s
  `docs/handoff/6502-BIOS.md` (branch `v3-vdp`). Existing jump-table addresses stay put.
  A later BIOS redesign may revise this.

## Decisions already made

- No new repositories.
- **6502-EMULATOR** makes the video card an option (TMS9918A or PICOVDP): one app, one
  site. It also publishes a frozen 2.6.9 web build at a versioned path for the legacy docs.
- **6502-DOCS** is versioned: legacy docs are frozen at `/6502-DOCS/v1/`, and the main
  site is rewritten for the VDP.
- **6502-BIOS** gets a `v1.x` maintenance branch; `main` becomes 2.x.
- **Assembly and C projects** get a VDP include chosen by a build option, not branches.
- **EhBASIC and vc83basic** stay 1.x. **PicoCalc** stays legacy. **The YouTube series**
  teaches the legacy VDP and mentions the new features.

## Order across the workspace

1. **6502-PICOVDP:** firmware proven on the PRO (its Phases 9–11). This gates the
   hardware switch, not the software work.
2. **6502-EMULATOR:** frozen 2.6.9 web build at `/6502-EMULATOR/v2/`.
3. **6502-DOCS:** `v1` branch published at `/6502-DOCS/v1/`, embeds pinned to step 2.
4. **6502-EMULATOR:** `v3-vdp` merged, with the card as an option; tagged 3.x.
5. **6502-BIOS:** `v1.x` cut; 2.0 built on `main`. This can start any time, because the
   `v3-vdp` emulator already runs the PICOVDP.
6. **6502-ASM** sets the VDP include convention. 6502-CRT, 6502-PRG, 6502-BIN and 6502-C
   follow it.
7. The emulator bundles BIOS 2.0. 6502-DOCS `main` is rewritten. 6502-ACE, bastok,
   WIZARDSLAB, 6502-EHBASIC, vc83basic and 6502-ASSEMBLY follow.

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
2. **Follow the emulator's merge.** When `v3-vdp` merges and the card becomes an option,
   these break:
   - `tools/sync-oracle.mjs` (via `tools/lib/emulator.mjs` `EMULATOR_BRANCH`) refuses any
     branch but `v3-vdp`. Move it to `main` or to a tag.
   - `host/node/Video.cjs` is presented as the emulator's `src/core/IO/Video`, mapped by
     the emulator's `jest.picovdp.cjs`. If the emulator renames the PICOVDP card, update
     the adapter, and the references in `README.md`, `docs/TRACE.md` and the tools.
   - Re-sync `tests/oracle/` from the merged commit. The goldens should not move, and
     re-syncing proves it.
3. **One spec.** `SPEC.md` here duplicates `6502-EMULATOR/docs/VDP-SPEC.md`, and
   `docs/SPEC.html` is rendered from it. Agree which is the source and how the copy is
   kept in step.
4. **The detection contract.**
   - BIOS 2.0 will detect this card through §16 (`STAT4` → `$AC`) and may keep a copy of
     `STAT6`'s capability bits.
   - After that, those values are ABI. Changing them is a BIOS change too.
5. **A release people can install.** A versioned UF2 on GitHub releases, with flashing
   instructions. 6502-ACE's README and 6502-DOCS's getting-started pages link to it, and
   both need the "RP2040 boards (v1.0–1.3) cannot run this" note (§2).
6. **Optional bench acceptance:** BIOS 2.0 booting and scrolling on the PRO. It is the
   first software besides the goldens to use `L0SCRY` on silicon.

## Linked repositories

| Repository | Path | Why |
|---|---|---|
| 6502-EMULATOR | `~/Developer/NodeJS/6502-EMULATOR` | The oracle and the reference implementation; branch name and file path coupling |
| 6502-BIOS | `~/Developer/Assembly/6502-BIOS` | §16 detection and §17's Kernal changes become BIOS 2.0 |
| 6502-DOCS | `~/Developer/NodeJS/6502-DOCS` | New-mode chapters are written from `SPEC.md`; flashing instructions |
| 6502-ACE | `~/Developer/Kicad/6502-ACE` | Hardware requirement and firmware link |
| 6502-ASM (then CRT, PRG, BIN, C) | `~/Developer/Assembly/6502-ASM` | The VDP include names §5's registers and §4's ports |

## Questions for VDP-PLAN.md

1. The canonical spec location and the sync direction.
2. The firmware version scheme versus spec drafts, and what version BIOS 2.0 requires.
3. Whether the oracle pins a tag instead of a branch after the merge.
