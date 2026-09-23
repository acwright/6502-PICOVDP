Phase 9 — Bench bring-up with the stock firmware
================================================

**Status:** done on 2026-09-23. Every check below passed.

Every criterion in PLAN.md's Phase 9 is met on the PRO's stock TMS9918A
firmware, before any of this repo's firmware has touched the card.

**The headline.**

- **The wiring is right.** A walking one and its complements round-tripped
  through VRAM 14/14 at every profile, which is what would have caught the
  CD0-is-the-MSB reversal PLAN.md section 5 warns about.
- **The picture matches exactly.** 46,080 of 46,080 pixels agree between the
  captured screen and a reference rendered from VRAM read back over the bus.
- **The card outran the harness.** No strobe, setup, hold or gap setting the
  Nano can produce caused a single error, so the Phase 11 baseline is the
  harness's floor rather than the card's limit.
- **The PRO is a 59.94 Hz part**, measured: 16.6838 ms between `/INT` edges,
  0.003% from 262 lines at 59.94 Hz and 0.10% from the emulator's exact 60 Hz.

Hardware: PICO9918 PRO v2.0 with the **HDMI dongle** (not the VGA dongle the
plan first assumed), FFC cable, HDMI capture card presenting as `USB Video`
at 640 × 480. Arduino Nano clone, CH340 bridge (`1a86:7523`), current
bootloader, on `/dev/cu.usbserial-1130`. Wired per
`bench/hardware/picovdp-bench.kicad_sch`, with the 220 Ω series resistors and
both 10 kΩ pull-ups fitted. The Phase 13 frame-sync tap is **not** fitted.

---

1. The captured screen shows the text
-------------------------------------

`vdpctl text` loads `fonts/cp437-6x8.bin` into the pattern table and puts up a
40 × 24 screen: a solid border of CP437 `$DB` all the way round, with text
inside. `vdpctl compare-capture` then reads the name and pattern tables **back
over the bus**, renders what they say should be on screen, grabs a frame, and
compares.

```
picture found at 82,47 480 x 384 in 640 x 480
46,080/46,080 pixels agree: 100.00%
```

100.00%, twice over, and the picture lands at exactly 480 × 384 — a clean 2× of
the 240 × 192 text area. The border is what makes this measurable: it puts ink
in the outermost row and column, so the bounding box *is* the text area and the
comparison needs no assumption about where the PRO places the active picture.

Deriving the reference from VRAM rather than from a golden file means this also
re-checks the 2 KB font and the 960-byte name table on every run.

2. 10⁶ random VRAM bytes, written and read back
------------------------------------------------

Random bytes over all 16 KB of VRAM, 62 passes, at both required profiles:

| Profile | Bytes | Wrong | Time | Rate |
|---|--:|--:|--:|--:|
| `6502-1mhz` | 1,015,808 | **0** | 85.4 s | 11,901 B/s |
| `fastest` | 1,015,808 | **0** | 74.5 s | 13,637 B/s |

The rate is set by host round trips, not the line rate — see section 6.

3. The /INT period
------------------

`vdpctl irq-timing` enables the vblank interrupt and catches 120 falling edges,
clearing the card's flag on the Nano after each so none is missed:

```
120 edges, 16.6838 ms apart (59.938 Hz)
against 16.6833 ms (59.94 Hz): +0.003%
```

**16.6838 ms**, 0.003% from 262 lines at 59.94 Hz. This is a direct measurement
of the difference PLAN.md's "Known, deliberate differences" records: the
emulator's 262 equal lines at exactly 60 Hz would be 16.6667 ms, which is 0.10%
away — thirty times the error seen here. The PRO is a 59.94 Hz part.

Clearing the flag has to happen on the Nano. A host round trip is about 4 ms
against a 16.68 ms frame, so from there some frames would be missed and the
average would come out a multiple of the period rather than the period.

4. Strobe-width and hold-time margins
-------------------------------------

`vdpctl sweep` writes and reads 1 KB of VRAM at each setting, holding the other
three axes at `6502-1mhz`:

```
  width  0:ok  1:ok  2:ok  3:ok  4:ok   least that works: 250 ns strobe
  hold   0:ok  1:ok  2:ok  3:ok  4:ok   least that works: 0 ns hold
  setup  0:ok  1:ok  2:ok  3:ok  4:ok   least that works: 0 ns setup
  gap    0:ok  1:ok  2:ok  3:ok  4:ok   least that works: 0 us apart
```

**The card accepted everything the harness can produce.** There is no failure
edge to record, so the Phase 11 baseline is the harness's floor rather than the
card's limit:

> A 250 ns `/CSW`, data sampled 187.5 ns after `/CSR` falls, no setup time, no
> hold time, and accesses back to back with no gap — all with zero errors.

250 ns is the Nano's floor, which is a 2 MHz 6502's strobe width. Phase 11 must
be no worse than this; it cannot be asked to be better, because the harness
cannot ask the question.

### The profiles had to be recalibrated first

The first sweep showed the same all-ok result for a bad reason. Left to itself
the compiler emitted a `call` for the delay helper and an `lds` for its count
**inside** the strobe, putting 14 cycles — 875 ns — between the two `PORTC`
writes even with the count at zero. `6502-1mhz` was really 1312 ns and
`fastest` was 875 ns: both *slower* than a 1 MHz 6502, so no sweep could ever
have found an edge.

The strobe path is now force-inlined with its operands in registers beforehand,
and the profiles are calibrated from the compiled instruction sequence:

| | `/CSW` low | data sampled after `/CSR` falls |
|---|--:|--:|
| width 0 | 4 cycles = 250 ns | 3 cycles = 187.5 ns |
| width n | 3n + 2 cycles | 3n + 3 cycles |

`6502-1mhz` is width 2 — 500 ns, as intended. `6502-2mhz` and `fastest` share
width 0 and differ only in setup, hold and gap, because 250 ns is the floor.

5. The serial rate
------------------

**1,000,000 baud, confirmed.** The 500,000 fallback is not needed.

6. 100 port opens and closes, with no stray access
--------------------------------------------------

```
after 100 resets: 16384/16384 bytes unchanged, status $80
no stray access
```

Every open pulses DTR and resets the Nano; while it is in reset each pin floats
and only the 10 kΩ pull-ups hold `/CSW` and `/CSR` high. All 16 KB came back
byte for byte.

Register readback is not part of this: a TMS9918A's control registers are write
only, so the status port is all there is to compare. Full register state waits
for `SNAPSHOT` over the debug link in Phase 10.

---

Not proved here, and cannot be
------------------------------

**MODE1 is untested.** The stock firmware is a TMS9918A: it decodes MODE alone
and ignores MODE1, so ports 2 and 3 behave as ports 0 and 1. The soldered `MDE1`
pin and its wiring stay unproven until this repo's firmware decodes all four
ports in Phase 11.

---

Three things that bit, and what they cost
-----------------------------------------

### macOS cannot set 1 Mbaud from `stty`

`tools/lib/link.mjs` opens the debug link as a tty and sets it raw with `stty`,
with no dependency, which works only because USB CDC's baud rate is nominal.
The Nano's is not. macOS `stty` takes 115200, 230400, 460800 and 921600; a
16 MHz AVR divides exactly to 1M, 500k, 250k and 200k. **The two sets do not
intersect** — 460800 and 921600 are 8–13% off on a 16 MHz AVR and 230400 is
3.5% off, outside what a UART tolerates. Only 115200 is settable and reachable,
at nine times the transfer cost.

So `tools/` gained its first dependency, `serialport`, exactly as PLAN.md
section 5 anticipated.

### serialport's reader does not wake the event loop

On Node 26 with serialport 13 and this CH340 bridge, bytes are delivered only
when the loop is woken for some other reason. A reply sat unseen until the next
timer fired — which is why the first run "timed out" at exactly 4000 ms and
then received its answer 1 ms later. pyserial on the same port is prompt, so
this is the binding and not the driver.

`Nano` keeps a 1 ms timer running while a request is in flight. Round trips are
about 4 ms, which is what sets the soak rate in section 2. If it is ever worth
dropping the workaround, the experiment is to re-run that measurement under
Node 22.

### The receive ring cannot be raised past 256

A 240-byte block makes a 247-byte packet, which fits the Arduino core's
256-byte receive ring. 254-byte blocks make 262-byte packets, which do not:
they worked in isolation and desynced after 131,072 bytes of sustained load.

Raising `SERIAL_RX_BUFFER_SIZE` to 320 is **not** the fix. Past 256 the core
switches its ring indices to `uint16_t`, and on this part serial then fails
outright — every command, including a 7-byte PING, came back `ERR_TIMEOUT`
after the Nano had read the header and never saw the two CRC bytes. Reverting
to 256 restored it immediately.

`BLOCK_MAX` is therefore 240, on both sides, and the largest packet is 247
bytes.

---

The stock flash
---------------

Still not backed up. Phase 9 does not overwrite it; **Phase 10 does**.
`picotool save` in BOOTSEL, kept outside the repo, before Phase 10 starts.
