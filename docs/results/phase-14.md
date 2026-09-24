Phase 14 — In the AC6502
========================

**Status:** done on 2026-09-24. Every criterion in PLAN.md's Phase 14 is met on
an AC6502 ACE with the PICO9918 PRO running this repository's `pro-release`
build, the first release: `STAT5` = `$10`, firmware 1.0. It was run at both
settings of the ACE's J1 PHI2 SELECT jumper, 1 MHz and 2 MHz, and under both
BIOS 1.6 and BIOS 2.0. The release is tagged `v1.0.0` in this repository and
not pushed.

The card was driven through the machine itself this time, not the Nano: the
ACE's own W65C02S, its serial console, and the capture card. There is no USB on
a release build and no cartridge slot on the bench, so everything that ran was
typed at BASIC or loaded over XMODEM from the Mac (`tools/acectl.mjs`), and
everything judged was either read back over serial or captured off the monitor.

**The headline.**

- **Both BIOSes boot unmodified to `OK`** at 1 MHz and at 2 MHz. BIOS 1.6's
  legacy text mode matches the emulator's run of the same ROM on the PICOVDP
  with no card pixel wrong; BIOS 2.0's boot, `screenful` and `scroll` match the
  `bios` goldens with no card pixel wrong, and the scroll is done in hardware:
  the name table read back with `VPEEK` equals the emulator's, whose Kernal
  scrolled through `L0SCRY` = 32, in all 960 bytes.
- **§16's detection, run by the 6502:** SPEC.md's own `DetectVdp` and
  `DetectFont`, copied as they stand, returned carry set 10,000 times in
  10,000, in every configuration. Both port pairs read `STAT4`–`STAT6` as
  `$AC $10 $BF`.
- **A real CPU's tightest accesses, on the real bus:** 500 passes at each
  clock of writes, reads, reads straight after the address command and status
  reads, each 4 cycles after the last — 2 µs at 2 MHz — on both pairs, and a
  VRAM-to-VRAM copy between them. **0 wrong** in over 7 million accesses a run.
- **`graphics-1.asm` draws what it drew** under BIOS 1.6, at both clocks: every
  checker of its 768 cells right in brightness.
- **VDP Modes and VDP Layers match their goldens** — all eight checkpoints,
  from the cartridges' own source run from RAM.
- **Three things about the machine, none of them the card:** at 2 MHz this ACE
  sometimes misses its SID and CompactFlash card at boot; under BIOS 1.6 at
  2 MHz its serial input sometimes replays old input; and BIOS 2.0 leaves a
  legacy program run from BASIC in the new Text mode, where `graphics-1.asm`
  draws nothing ([Found on the way](#found-on-the-way)).

Contents: [The machine](#the-machine) ·
[What was built](#what-was-built) ·
[The runs](#the-runs) ·
[BIOS 2.0](#bios-20) · [BIOS 1.6](#bios-16) ·
[The probe and the bus](#the-probe-and-the-bus) ·
[The cartridges](#the-cartridges) ·
[A wrong glyph](#a-wrong-glyph) ·
[Found on the way](#found-on-the-way) ·
[For 6502-ACE](#for-6502-ace) ·
[Done when](#done-when) · [Differences from the plan](#differences-from-the-plan) ·
[After this plan](#after-this-plan) · [Reproducing](#reproducing)

---

The machine
-----------

| | |
|---|---|
| Computer | AC6502 ACE, with the BAT85 on `/INT` that keeps the PRO's push-pull `/INT` off the shared `IRQB` |
| Clock | J1 at 1 MHz, then 2 MHz, then 1 MHz: measured by the bus test against the card's frame at **0.9997** and **1.9996 MHz** |
| Video | PICO9918 PRO v2.0 with its `MDE1` pin, HDMI dongle, capture card |
| Firmware | `pro-release`, `STAT5` `$10`. `picovdp.uf2` SHA-256 `0b74cd9875cbffc46de7ec48a9bb6e0d4a76a3e796d2563615e17e6bed94afd5`, `picovdp.elf` `e716e12bf85ecfaf19eb759b212333518fb5c729fad49f771f00fac39b06f1d8`. Flashed from BOOTSEL with `picotool load -v -x -f` (picotool 2.3.0), verified |
| BIOS 2.0 | **v2.0.2**, `7a71252d…`: `$A000`–`$FFFF` read back from the machine with `BSAVE` over XMODEM, equal to 6502-BIOS's `v2.0.2` `BIOS.bin` and to the emulator's `BIOS2.bin`, the ROM the goldens were captured with |
| BIOS 1.6 | swapped in for its runs. Its pictures match the emulator's bundled `BIOS.bin` (`4b4154af…`, 1.6 as reissued); the ROM itself was not read back |
| Serial | the ACE's R6551AP, 19200 8-N-1, a USB serial bridge on the Mac |
| Cards found | at 1 MHz `HW_PRESENT` `$FD`: RAM, RTC, CF, SER, VIA, SID, VDP. At 2 MHz CF and SID are sometimes missing ([Found on the way](#found-on-the-way)) |

---

What was built
--------------

**The release.** `firmware/renderer.h` sets `PICOVDP_VERSION_BCD` to `$10`:
§6's `STAT5`, high nibble major, low minor, so firmware 1.0.
`tools/lib/bus-replay.mjs` reads that line for what a release build, which has
no link to ask, reports — so `vdpctl replay --no-link` expects `$10` now
without being told.

**`tests/machine/`** — the programs the machine runs, each a `.prg` that
BASIC's `LOAD` takes over XMODEM (`make -C tests/machine`, into
`build/machine/` with a label file beside each):

- `probe.asm` — SPEC.md §16's `DetectVdp` and `DetectFont` verbatim, 10,000
  times each with interrupts off, and one plain read of `STAT4`–`STAT6` through
  each pair.
- `bus.asm` — the clock, and then a 6502's tightest accesses on both pairs
  ([The probe and the bus](#the-probe-and-the-bus)).
- `vdp-modes.asm`, `vdp-layers.asm` — the emulator's `VdpModes.asm` and
  `VdpLayers.asm`, included unchanged after the BASIC stub by `cart.inc`
  ([The cartridges](#the-cartridges)).
- `graphics-1.prg` — 6502-DOCS's `samples/assembly/graphics-1.asm`, taken from
  the frozen `v1` branch with `git show` and built against the legacy
  `6502.inc` it was written for. BIOS 2.0 keeps every entry point it calls.

**`tools/acectl.mjs`** and its libraries:

- `lib/ace.mjs` — the ACE's serial console. BIOS 2.0 prints only to the screen
  while a card is fitted, so a value comes back by running its `PRINT` with
  `IO_MODE` (`$0306`) set to serial for one line; `.prg` files go by XMODEM
  (`LOAD`), memory comes back by XMODEM (`BSAVE`), and a program says it has
  finished by sending one byte with `SerialChrout`. No serialport dependency:
  the tty is set raw with `stty` and read through Node's tty stream, as
  `lib/link.mjs` does.
- `lib/machine.mjs` — the checks: `probe`, `bus`, `bios`, `graphics-1`, `cart`.
- `lib/machine-ref.mjs` — the emulator's compiled engine run the same way, for
  every picture the oracle has no golden of: a ROM, a `.prg` loaded as `LOAD`
  leaves it, keys typed at the serial card, and captures in a golden's shape.
- `lib/capture.mjs` gains `stream`, every frame at 60 a second, and `grabMean`,
  a still picture as the mean of eight frames.
- `lib/screen.mjs` gains `luma` and `wrongBlocks`, which find one wrong glyph
  ([A wrong glyph](#a-wrong-glyph)).

The bus test was checked in the emulator before it met the machine: on
`Video.ts` as it is, every counter 0; on a `Video.ts` made to corrupt every
97th data read and every 89th status read, every counter counts.

---

The runs
--------

Four configurations, each on the release, each with the ACE powered on fresh
into it. Raw results are in `docs/results/phase-14/`.

| | BIOS 2.0, 1 MHz | BIOS 2.0, 2 MHz | BIOS 1.6, 1 MHz | BIOS 1.6, 2 MHz |
|---|---|---|---|---|
| Power-on to `OK`, captured before any key | ✅ 0 card pixels wrong | ✅ 0 | ✅ 0 | ✅ 0 |
| The boot and both inputs, after a restart | ✅ 0, 0, 0 | ✅ 0, 0, 0 | ✅ 0, 0, 0 | ✅ 0, 0, 0 |
| The scroll in hardware | ✅ 960 of 960 bytes | — | — | — |
| `graphics-1.asm` | draws nothing, as the emulator does | — | ✅ | ✅ |
| §16's probe, 10,000 each | ✅ | ✅ | ✅ | ✅ in one run of three |
| The bus test | ✅ 500 passes, 0 wrong | ✅ 500, 0 | ✅ 64, 0 | ✅ 500, 0 |
| VDP Modes, VDP Layers | ✅ 8 of 8 | ✅ 8 of 8 | — | — |
| Replayed input | none | none | none | several |

The carts need BIOS 2.0's `KernalInit`, and `graphics-1.asm` draws only under
1.6, so those cells are empty by design. Under BIOS 1.6 at 2 MHz the serial
input's replays twice stopped a run before the probe or the bus test finished;
the probe passed in the run that reached it, and the bus test's 500 passes ran
to their end and were read back after a reset.

---

BIOS 2.0
--------

The oracle's `bios` fixture on the machine: the boot, then
`FOR I=1 TO 9:PRINT "LINE";I:NEXT` for `screenful`, then `PRINT "SCROLLED"`,
which scrolls four rows, for `scroll`. The inputs are typed over serial, as
the fixture's are, and each picture is the mean of eight captured frames,
judged against its golden.

| Checkpoint | 1 MHz: mean error, settled pixels within 8 | card pixels wrong | 2 MHz | |
|---|---|--:|---|--:|
| `ok` | 2.05, 99.995% | 0 | 2.14, 99.995% | 0 |
| `screenful` | 2.59, 99.994% | 0 | 2.59, 99.994% | 0 |
| `scroll` | 1.94, 100% | 0 | 2.02, 100% | 0 |

The header on each reads `AC6502 BIOS v2.0`, `BASIC v2.0 30718 BYTES FREE`,
`RAM RTC CF SER VIA SID VDP` — `VDP` in it is the Kernal's §16 probe finding the
card by `STAT4` and `STAT6` b7. The screen straight after power-on, before
anything is typed, matches the `ok` golden too, at both clocks
(`shots/bios-2.0-1mhz-power-on.png`); at 2 MHz that boot had found neither CF
nor SID and is judged against the emulator's boot without them, which it
matches with no card pixel wrong.

**The scroll is the card's.** A picture cannot say whether BIOS 2.0 scrolled
through `L0SCRY` or by moving every row in VRAM; VRAM can. After `scroll` the
name table, `$0000`–`$03BF`, is read back with `VPEEK` over serial, and compared
with the emulator's VRAM at the same moment — the emulator types the same line
and is read as it sends its first byte. Its Kernal scrolled through `L0SCRY`,
which the golden records at 32. **All 960 bytes are equal.** A software scroll
of the same four rows would move 291 of them.

Two things about restarting it, both the BIOS's own:

- **A reset keeps a program at `$0800`**, and BASIC counts it
  (`Kernal.asm`: "A reset is a cold BASIC start"). After a `.prg` the header
  reads 30708 bytes free, not 30718. The check types `NEW` first.
- A restart is `SYS` to the address in the reset vector, not the button: the
  Kernal sets the card up again from its registers, though the card's RST pin
  is not pulsed. The power-on captures are the check that a cold machine does
  the same.

---

BIOS 1.6
--------

BIOS 1.6 knows only the TMS9918A: its console is legacy Text mode, `VMODE` 0,
and its 2 KB character set upload lands on the card's own font. No golden holds
it on the PICOVDP — the oracle's run of this ROM is on the TMS9918A
(`tms9918a/bios`) — so the reference is the emulator running 1.6 on the
PICOVDP, with that fixture's inputs: `FOR I=1 TO 15`, then `PRINT "SCROLLED"`,
which 1.6 scrolls in software.

| Checkpoint | 1 MHz: mean error, settled | card pixels wrong | 2 MHz | |
|---|---|--:|---|--:|
| power-on, before any key | 0.52, 100% | 0 | 0.46, 100% | 0 |
| `ok` after a restart | 0.57, 100% | 0 | 0.41, 100% | 0 |
| `screenful` | 1.20, 100% | 0 | 1.21, 100% | 0 |
| `scroll` | 0.93, 100% | 0 | 1.21, 100% | 0 |

1.6 keeps BASIC's warm-start flag (`BAS_WARM`, `$036F`) across a reset and then
prints only `OK`; the check clears it before restarting, so the restart is as
cold as a power-on.

**`graphics-1.asm`** — 32 × 24 cells, each a one-pixel checkerboard of one of
32 colour pairs, chosen by a seeded random fill — is judged against the
emulator's run of the same `.prg` on the same ROM. The path carries colour at
half resolution, and this is the one picture whose colour changes every card
pixel, so its whole-picture error is 47 levels, past Phase 10's gate of 32, at
both clocks. Brightness is carried whole, and read a card pixel at a time
([A wrong glyph](#a-wrong-glyph)) **every pixel of every cell is right**, at a
threshold of 32 levels as well as 64. The same picture moved one cell to the
side has 2,925 wrong. A key then returns to BASIC, at both clocks.

---

The probe and the bus
---------------------

**The probe** (`tests/machine/probe.asm`):

| | `DetectVdp` carry set | `DetectFont` carry set | port A `STAT4`–`6` | port B |
|---|--:|--:|---|---|
| every configuration | 10,000 of 10,000 | 10,000 of 10,000 | `$AC $10 $BF` | `$AC $10 $BF` |

Port B answering is also the check that the `MDE1` pin is fitted and reaches
the card. From BASIC, on BIOS 2.0:

```basic
POKE 39939,4:POKE 39939,142:PRINT PEEK(39939):POKE 39939,0:POKE 39939,142
```

prints 172 (`STAT4` through `$9C03`) and leaves port B's `STATSEL` at 0. Without
the pin, `$9C02`/`$9C03` are port A's addresses again and the same line reads
port A's `STAT0`.

**The bus test** (`tests/machine/bus.asm`). The 6502's tightest spacing is
4 cycles: `sty VC_DATA : sta VC_DATA`, `ldy VC_DATA : lda VC_DATA`,
`stx VC_REG : lda VC_DATA`. At 2 MHz that is Phase 13's 2 µs, made by a CPU
instead of the Nano. Each pass draws 256 random bytes and, through each pair in
turn, writes 4 KB of VRAM at `$4000` in pairs, reads it back in pairs, reads
1,024 random bytes of it straight after the address command that names each,
and reads `STAT4` 2,048 times in pairs; then port B reads the 4 KB while port A
writes it to `$5000`, 4 cycles apart, and it is read back. Interrupts are off
throughout; `VBANK`, `VINC` and both `STATSEL`s are put back.

| Run | Clock | Per pair: write pairs, tight reads, reads after the address | status reads | copied | wrong |
|---|--:|---|--:|--:|--:|
| BIOS 2.0, 1 MHz, 500 passes | 0.9997 MHz | 1,024,000 each, 512,000 | 1,024,000 | 2,048,000 | **0** |
| BIOS 2.0, 2 MHz, 500 passes | 1.9996 MHz | 1,024,000 each, 512,000 | 1,024,000 | 2,048,000 | **0** |
| BIOS 1.6, 2 MHz, 500 passes | 1.9996 MHz | the same | | | **0** |
| BIOS 1.6, 1 MHz, 64 passes | 0.9997 MHz | 131,072 each, 65,536 | 131,072 | 262,144 | **0** |

The clock is 25-cycle loops counted over 16 of the card's frames, which Phases
9 and 13 measured at 16.6838 ms.

---

The cartridges
--------------

The bench's AC6502 has no way to put a `.crt` in its slot, and the two sample
cartridges don't need one: each is about 1 KB of code at `$C000` that calls
only `KernalInit` and keeps interrupts off. `tests/machine/cart.inc` puts the
cartridge's own source after the BASIC stub, unchanged, and `RUN` enters at
`CartReset` as a cold reset would. The committed `.crt` files reassemble from
those sources byte for byte, so it is the same code at another address. The
one addition is a way out: the wrapper routes the first `jsr` at `Main`
through a check of the serial port, and a byte arriving there takes the
machine back to BASIC through the reset vector.

A cartridge's pictures move — VDP Modes holds each of its four screens for a
second, VDP Layers scrolls every frame and comes back to a state every 1,920
frames — so the capture card is watched at 60 frames a second, for 12 and 70
seconds, and each checkpoint is judged on the captured frame most like it: the
matching visual state (§18: the frame numbers will not line up).

| Checkpoint | 1 MHz: mean error, settled | card pixels wrong | 2 MHz | |
|---|---|--:|---|--:|
| `vdp-modes/text` | 22.1, 99.99% | 0 | 22.1, 99.99% | 0 |
| `vdp-modes/compact` | 17.7, 100% | 0 | 17.7, 100% | 0 |
| `vdp-modes/graphics` | 21.9, 100% | 0 | 21.9, 100% | 0 |
| `vdp-modes/full` | 27.5, no settled pixel | 0 | 27.5 | 0 |
| `vdp-layers/parallax` | 10.0, 99.97% | 0 | 10.0, 99.97% | 0 |
| `vdp-layers/scroll-bit8-l1` | 10.1, 99.85% | 0 | 10.1, 99.84% | 0 |
| `vdp-layers/occluded` | 10.3, 99.78% | 0 | 10.3, 99.77% | 0 |
| `vdp-layers/scroll-bit8-l0` | 10.2, 99.94% | 0 | 10.2, 99.96% | 0 |

The 2 MHz VDP Modes figures are from its second watch: in the full run the
stream reported 256,000 frames in 12 seconds and never showed the last two
screens — the capture's fault, since the cartridge went round and left when
asked — and `checkCart` now watches again rather than judge a stream like that.

---

A wrong glyph
-------------

Phase 10's tolerance judges the settled pixels, those at least 8 capture pixels
from any colour change. On a text screen almost none of a glyph's pixels are
settled, and one glyph is a twentieth of a percent of the screen: the first
BIOS 2.0 capture read `30708 BYTES FREE` against a golden reading `30718`, and
passed. So every picture here is also judged a card pixel at a time.

`wrongBlocks` reads both pictures as brightness — the path carries brightness
at full resolution and colour at half — and each card pixel of the capture as
the mean of its 2 × 2 capture pixels. A card pixel is wrong if it is more than
64 levels from the golden's brightness there, through the picture's own
calibration, and from every even blend of that with a neighbour's: once a block
is aligned the path is within half a card pixel of it, so an edge may read as
either side, but a stroke where the golden has none cannot. Each 8 × 8 block of
card pixels is read at the picture's offset and every other within 2 capture
pixels, and keeps its fewest wrong, because the dongle's scale is not exactly 2
and a line of text drifts by a pixel across the screen. A block with 3 or more
wrong is a problem.

What it was set on, the BIOS 2.0 `ok` capture against its golden with glyphs
exchanged in the golden:

| The golden changed | wrong card pixels in the block |
|---|--:|
| nothing | 0–2 |
| `1` → `0` in `30718` | 12 |
| `C` → `O` | 13 |
| `E` → `F`, `S` → `5` | 3 |
| `8` → `B` | 0: two corner pixels, which the path's blur cannot separate |

On every capture in this phase that was right, no block reached 3: 0 in every
cartridge and `graphics-1.asm` picture, at most 2 on a text screen. The gate is
`BLOCK_TOLERANCE` in `tools/lib/screen.mjs`.

---

Found on the way
----------------

1. **BIOS 2.0 and a legacy program run from BASIC.** `graphics-1.asm` writes
   registers 0–7 for Graphics I and turns the display on. Under BIOS 1.6 that is
   what the card shows. Under BIOS 2.0 the console has put the card in Text mode,
   `VMODE` 1, where registers 0–7 do not choose the mode, so the screen is the
   backdrop — black — and it is the same in the emulator. BIOS decision 11 keeps
   legacy *cartridges* working, because `KernalInit` leaves `VMODE` 0 until the
   first console call; a `.prg` started from BASIC 2.0 comes after one. SPEC.md
   §17's promise — "the BIOS boots untouched and `graphics-1.asm` still runs" —
   is about the unmodified BIOS, 1.6, and holds. Whether BIOS 2.0 should put
   `VMODE` back to 0 before `SYS`, or legacy programs should write it, is
   6502-BIOS's decision.
2. **This ACE at 2 MHz finds its SID and CF card only sometimes.** At 1 MHz both
   are found (`HW_PRESENT` `$FD`; the SID was missed once in about a dozen boots).
   At 2 MHz, under both BIOSes, boots came up with `$B5` — no SID and no CF —
   and some with both. The check judges such a boot against the emulator's
   machine without them. It is the ACE's business: a SID is a 1 MHz part, and
   neither card is on the card's bus.
3. **Under BIOS 1.6 at 2 MHz, the ACE's serial input sometimes replays old
   input.** After a transfer, or a program, BASIC ran lines typed seconds or
   minutes before — the input ring, `$0200`, survives a reset — and twice ran an
   old `LOAD` and waited in it. 1.6 keeps the ring's pointers at `$00` and `$01`;
   the lengths replayed, from five bytes to about 230, say the read pointer took
   a wrong value. What was established:
   - it happened only at 2 MHz under BIOS 1.6: never at 1 MHz under either BIOS,
     never under BIOS 2.0 at 2 MHz, in the same checks;
   - it is not a read the card corrupted: the bus test ran 500 passes in the
     same configuration, 10⁷ card accesses among its RAM reads and compares,
     with nothing wrong;
   - the emulator running 1.6 with the same XMODEM stream at 2 MHz spacing did
     not do it in 20 tries, but models neither the ACIA's transmit time nor bus
     timing;
   - with the video card deselected by its DIP switch it did not recur in 6
     tries, nor with the card enabled in 6 more after the R6551AP was taken out
     and refitted, and then it recurred;
   - the boot text over serial was garbled at 2 MHz with the card deselected;
   - an R65C51P2 tried in its place received nothing at all.

   So it is the ACE's serial path at 2 MHz under BIOS 1.6, and what exactly —
   the ACIA's bus timing, its socket, or a race 1.6 has and 2.0 does not — needs
   a scope on the ACIA. `acectl` notices a replay after a load, cancels a
   replayed `LOAD`, and records it.
4. **Once, the capture stream went wrong:** 256,000 frames reported in 12
   seconds, the last two VDP Modes screens never seen. The second watch was
   right, and a stream that cannot be right is now watched again.

---

For 6502-ACE
------------

6502-ACE's `VDP-PLAN.md` section 7 lists what this gate hands over. From this
phase:

1. **The release:** tag `v1.0.0`, `STAT5` `$10`, SPEC.md draft 0.5, the
   image's SHA-256 above ([The machine](#the-machine)). The tag is local; the
   GitHub release and its `.uf2` asset wait for the owner's push.
2. **Phase 14 in an ACE with that image:** `OK` at both J1 settings with `VDP`
   in the hardware line — header text above, captures in
   `docs/results/phase-14/shots/`. The BIOS in U8 was **v2.0.2**, not the
   `v2.0` (`4702fad7…`) the plan names. Detection held at every power-on and
   every restart; the restarts were through the reset vector — the button was
   pressed only under BIOS 1.6.
3. **`MDE1` in the machine:** port B answers in every run; the BASIC line in
   [The probe and the bus](#the-probe-and-the-bus) is the builder's check.
4. **Flashing:** BOOTSEL and `picotool load -v -x -f` with picotool 2.3.0; the
   release build has no USB, so going back to it means BOOTSEL again. Whether
   the PRO may be flashed in the ACE's socket is for the owner to record.
5. **Stock firmware:** saved in Phase 9 (`~/Developer/Backups/pico9918-pro/`);
   a restore was not tried.
6. **Board-level findings:** the diode on `/INT`, found before this phase and
   fitted; and [Found on the way](#found-on-the-way) 2 and 3, the ACE at 2 MHz.
7. **Enclosure access:** not checked.

---

Done when
---------

PLAN.md's Phase 14:

| Check | Result |
|---|---|
| The PRO, with its `MDE1` pin, fitted to the AC6502; `pro-release` with `STAT5` set to the release version | ✅ `STAT5` `$10`, read through both pairs; port B works |
| §16's detection probe returns carry set | ✅ 10,000 of 10,000, `DetectVdp` and `DetectFont` as SPEC.md prints them, at both clocks under both BIOSes |
| the unmodified BIOS 1.6 reaches `OK` in its legacy text mode, judged on the bench | ✅ at 1 and 2 MHz, from power-on and from a restart; 0 card pixels wrong against the emulator's run of the ROM on the PICOVDP |
| BIOS 2.0 reaches `OK`, detecting the card by `STAT4` `$AC` and `STAT6` b7 | ✅ `VDP` in its hardware line, at both clocks |
| the `screenful` and `scroll` inputs show what the `bios` goldens show, the scroll in hardware through `L0SCRY` | ✅ 0 card pixels wrong at both clocks; the name table equals the `L0SCRY`-scrolled emulator's in all 960 bytes |
| `graphics-1.asm` draws what it drew | ✅ under BIOS 1.6 at both clocks, every cell right; under 2.0 it draws nothing, as in the emulator ([Found on the way](#found-on-the-way) 1) |
| `VdpModes.crt` and `VdpLayers.crt` match their goldens' pictures within the capture tolerance, at matching visual states | ✅ all eight checkpoints at both clocks, from the cartridges' own source run from RAM |
| the release is tagged, and 6502-EMULATOR's plan is told its merge condition is met | ✅ `v1.0.0`, local; the emulator's `VDP-PLAN.md` §9.4 records that the PICOVDP part of its gate is met |
| the host suite passes, the firmware presets build | ✅ 28 of 28; `pro-release` and `pro-debug` build |

---

Differences from the plan
-------------------------

1. **The cartridges ran from RAM.** Their unchanged source, after a BASIC stub,
   with one hook to leave; the `.crt` images reassemble from that source byte
   for byte. The owner offered to make a cartridge if one was needed; it was
   not.
2. **"Typed by hand" was typed by the Mac,** over the serial console, as the
   fixture's own keys go to the serial card. The machine was restarted through
   its reset vector; the power-on captures stand for the cold boot.
3. **`graphics-1.asm` was judged under BIOS 1.6,** the unmodified BIOS SPEC.md
   §17 means, and against the emulator's run of it, there being no golden. Its
   whole-picture error is recorded, not gated: its colour changes every card
   pixel, finer than the path carries. It is judged a card pixel at a time in
   brightness instead.
4. **BIOS 1.6 is judged against the emulator's run of it on the PICOVDP**, since
   no golden of it on this card exists.
5. **Every picture is also judged block by block** ([A wrong glyph](#a-wrong-glyph)),
   which Phase 10's tolerance cannot do for text.
6. **Beyond the plan:** the probe and the bus test on the real CPU at both
   clocks; BIOS 1.6 at 2 MHz; the VRAM check of the hardware scroll.
7. **The tag is local.** Pushing, and a GitHub release with the image as an
   asset, are the owner's.

---

After this plan
---------------

- **6502-BIOS:** hardware `VideoScroll` is already in 2.0, and proven above.
  What is left is SPEC §17's list, and [Found on the way](#found-on-the-way) 1:
  whether a legacy program started from BASIC 2.0 should find `VMODE` 0.
- **6502-EMULATOR:** §9.4's default flip. Its gate — the firmware proven on the
  PRO, BIOS 2.0 released, the DOCS rewrite published — is met.
- **6502-ACE:** D3, with section [For 6502-ACE](#for-6502-ace)'s records; and
  the ACE at 2 MHz, [Found on the way](#found-on-the-way) 2 and 3.

---

Reproducing
-----------

With the ACE's serial port on the Mac (`ACE_PORT`, or the first
`/dev/cu.usbserial-*`) and the capture card as device 0:

```sh
cmake --preset pro-release && cmake --build --preset pro-release
picotool load -v -x -f build/pro-release/firmware/picovdp.uf2   # the PRO in BOOTSEL
make -C tests/machine
(cd ../../NodeJS/6502-EMULATOR && npm run build:cli)             # the references

# BIOS 2.0, at each J1 setting
node tools/acectl.mjs info
node tools/acectl.mjs all --version '$10' --passes 500 --pictures shots --out bios-2.0.json
node tools/acectl.mjs graphics-1                                 # draws nothing, as the emulator does

# BIOS 1.6, at each J1 setting
node tools/acectl.mjs all-1.6 --version '$10' --passes 500 --pictures shots --out bios-1.6.json
```

Raw results are in `docs/results/phase-14/`: one JSON per configuration, the
2 MHz VDP Modes watch again, the 500-pass bus run under BIOS 1.6 at 2 MHz, the
VRAM check of the scroll, and in `shots/` the captures — the cartridges' halved
to the card's 320 × 240.
