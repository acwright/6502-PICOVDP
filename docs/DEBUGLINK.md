The Debug Link
==============

**Protocol version 1.** How the firmware's debug builds (`pico2`, `pro-debug`)
are driven and inspected over their USB-C port, with no SWD (PLAN.md section 3,
"The debug link"). `tools/vdpctl.mjs` is the host side; `firmware/link.c` the
board's.

In this document, **§** means a section of SPEC.md.

---

Contents
--------

1. [Transport](#1-transport)
2. [Packets](#2-packets)
3. [Commands](#3-commands)
4. [The Injection Stream](#4-the-injection-stream)
5. [Snapshots](#5-snapshots)
6. [Faults, the Watchdog and Safe Mode](#6-faults-the-watchdog-and-safe-mode)
7. [Where the Link Runs](#7-where-the-link-runs)
8. [vdpctl](#8-vdpctl)

---

1. Transport
------------

USB CDC, through the SDK's `pico_stdio_usb` with CRLF translation off. The
board enumerates as `/dev/cu.usbmodem*` on macOS; `vdpctl` takes the first, or
`PICOVDP_PORT`.

- **The baud rate is nominal**, except 1200: `pico_stdio_usb` reboots the board
  into BOOTSEL when the host selects it. `vdpctl` sets 115200.
- **The host must hold DTR**, which opening the port does. The board writes
  nothing while no host is connected, and discards what it cannot write within
  500 ms.
- **Flashing** is `picotool load -x -f`: `pico_stdio_usb`'s reset interface lets
  picotool reboot a running board into BOOTSEL, load, and start the new image.
  Only the first flash of a board needs the BOOT button.
- **Nothing else is written** to the port. The firmware never calls `printf`;
  log text, if any, travels as its own packet type.

---

2. Packets
----------

Both directions use one framing. Every integer is little-endian.

| Offset | Size | Field |
|--:|--:|---|
| 0 | 2 | `PV` (`$50 $56`) |
| 2 | 1 | type |
| 3 | 1 | sequence: the host's choice, echoed in the answer |
| 4 | 4 | payload length, at most 16,384 from the host |
| 8 | length | payload |
| 8 + length | 4 | CRC-32 (IEEE, as zlib's) of bytes 2 to the end of the payload |

A receiver hunts for `PV`, reads the header, and drops a packet whose CRC does
not match. The board answers a bad CRC with an ERROR.

**Types.** A command is `$01`–`$7E`. Its answer is the command with bit 7 set.
`$FF` is ERROR: the payload is the reason, as text, and it answers the command
whose sequence it carries. `$7F` is LOG, unsolicited text; version 1 sends none.

The board answers each command before it reads the next. A command that waits,
such as SNAPSHOT, holds the ones behind it.

---

3. Commands
-----------

Every field below is the payload's, in order. A text field is NUL-padded to its
size.

### INFO `$01`

No payload. Answers in normal mode and in safe mode.

| Field | Size | |
|---|--:|---|
| protocol | 1 | 1 |
| safe mode | 1 | 1 in safe mode (section 6) |
| reset reason | 1 | 0 power-on or anything else, 1 our reboot after a fault record, 2 the watchdog |
| version | 1 | `STAT5`, BCD |
| clock | 4 | the system clock, Hz |
| uptime | 8 | µs |
| build | 32 | `git describe` of the tree the image was built from |
| board | 16 | `PICO_BOARD` |
| unique id | 24 | the flash's unique id, hex |
| record present | 1 | |
| record | 312 | if present: section 6's fault record |

### STATS `$02`

Payload: one byte, bit 0 to reset the statistics once they are read. Debug
builds only, like everything below. Cycles are the M33's DWT cycle counter on the
core named.

| Field | Size | |
|---|--:|---|
| uptime | 8 | µs |
| clock | 4 | Hz |
| budget | 4 | cycles in a display line at the PIO's rate: 22,372 at 352 MHz |
| rows built | 4 | picture rows |
| lines taken | 4 | latches caught up with, rows and blanking |
| late rows | 4 | picture rows whose line started before their build had finished (§18) |
| latches merged | 4 | latches the render side fell 8 behind on and merged: rows never built |
| missed bells | 4 | line starts core 1's latch took more than one of at once |
| journal overflows | 4 | catch-ups that copied VRAM pages |
| raster slips | 4 | line starts not following the line before |
| latency | 12 | max, 99.9th percentile, mean: core 1's latch interrupt to the row's buffer ready |
| build | 12 | max, 99.9th percentile (0), mean: core 1's catch-up to the buffer ready |
| stage maxima | 24 | core 1: catch-up, its half, its expansion, waiting for core 0, publishing; core 0: its half and its expansion |
| split mean | 4 | the picture column rows were divided at |
| split rows | 4 | rows divided |
| latch interrupt max | 4 | core 1 |
| line-start interrupt max | 4 | core 0 |
| bus stand-ins | 4 | LOAD's bus interrupts taken |
| bins | 2 | 512 |
| shift | 1 | 7: a bin is 128 cycles |
| histogram | 2 × bins | latency, saturating at 65,535 |

### SNAPSHOT `$03`

Payload: mode (1), frame (4), timeout in ms (4). Mode 0 captures the next frame
to start; 1 captures raster frame `frame`; 2 collects a capture already armed (by
INJECT). The answer is section 5's snapshot, or an ERROR on timeout.

### VRAM `$04`

No payload. The answer is the bus copy of VRAM, 65,536 bytes, copied by core 1
between two lines.

### INJECT `$05`

Payload: a subcommand byte, then its fields.

| Subcommand | Fields | Does |
|---|---|---|
| 0 BEGIN | capture frame (2) | starts an injection; captures trace frame `capture frame` as it goes to VGA, unless `$FFFF`. ERROR if one is running |
| 1 DATA | stream bytes | appends whole records (section 4) to the board's 32 KB ring |
| 2 STATUS | — | |
| 3 ABORT | — | ends the injection; the card goes back to taking latches in the interrupt |

Every subcommand answers with the status:

| Field | Size | |
|---|--:|---|
| state | 1 | 0 idle, 1 armed, 2 running, 3 ended, 4 failed |
| base frame | 4 | the raster frame the cold reset fell in: trace frame 0 |
| frame, line | 2 + 2 | the trace line last applied |
| operations | 4 | |
| reads | 4 | |
| STAT5 reads | 4 | reads not compared (section 4) |
| mismatches | 4 | |
| space | 4 | stream bytes the ring can take now |
| buffered | 4 | stream bytes waiting |
| taken | 4 | DATA: bytes accepted |
| end state | 165 | section 5's state, as the card stood at END |
| logged | 1 | up to 32 |
| log | 13 each | operation (4), frame (2), line (2), kind (1), port (1), expected (1), got (1), status selected (1, `$FF` for none) |

Mismatch kinds: 0 a read, 1 `/INT` after an operation, 2 `/INT` after a latch, 3
a record that arrived after its line had passed, 4 a record the board could not
parse.

### RESET `$06`

Payload: one byte, 1 for power-on. §15 on the card, between two lines.

### REBOOT `$07`

Payload: one byte, 1 for BOOTSEL. Answers, forgets the fault record, and
reboots. Answers in safe mode.

### FAULT `$08`

Payload: the kind — 0 a HardFault on core 0, 1 a HardFault on core 1, 2 core 1
hangs with interrupts off, 3 a panic. Answers, then raises it. Kinds 0 and 3 run
on core 0 and answer in safe mode too. Debug builds only.

### SCENE `$09`

Payload: a scene's name, as `firmware/scenes.h` lists them. From the next screen
line 250, the card is reset at power-on and the scene set up; every frame after,
its program reads status at screen line 250 and scrolls the layers. Answers the
scene's index (1) and name (32).

### LOAD `$0A`

Payload: bus rate in Hz (4), handicap first row (2), last row (2), every (2),
cycles (4). Zero turns each off.

- **The bus stand-in** is Phase 1's: a PWM interrupt on core 1 at the highest
  priority, doing a data write through port B into the palette window, of the
  byte already there, and every 512 writes setting the pointer back.
- **The handicap** pads the build of rows [first, last), every `every` rows, on
  core 1, to at least `cycles` from the start of the row: §18's late line, on
  purpose.

### SCENE LOG `$0B`

No payload. Answers the frames logged since the last SCENE LOG, oldest first,
at most 64: count (1), reads per frame (1, 11), then per frame the scene frame (4)
and what the program read: `STAT1`, `STAT7`, `STAT8`–`STAT15`, then `STAT0`.

### PROFILE `$0C`

Payload: a picture row (2), iterations (4). Core 1 runs each stage of that row
again and again with interrupts off, and answers the row (2), its sprite count
(1), two counter reads' cost (4), the stage count (1), then each stage's minimum
and maximum (4 + 4): evaluation, layer 0, layer 1, the whole row as one half,
the left half, the right half, the row's expansion, the split's choice. Latches
wait while it runs.

---

4. The Injection Stream
-----------------------

The program side of a trace (docs/TRACE.md), up to one checkpoint, as records:

| Record | Bytes | |
|---|--:|---|
| COLD | `$02`, screen line (2) | TRACE.md's `X cold`: the first record |
| LINE | `$01`, frame (2), screen line (2), `/INT` (1) | the operations after it fell in trace frame `frame`, after that screen line's latch; `/INT` is the level after the latch, or `$FF` for no check |
| operation | `$80` \| `$40` for a read \| `$20` if `/INT` is asserted after it \| port, then value | a write of `value`, or a read expected to return `value` |
| END | `$03` | the checkpoint |

`vdpctl inject` writes a LINE before a line's first operation, and for any line
whose latch moved `/INT` without one.

**How the board applies it.** While an injection runs, core 1 takes each latch
in its thread rather than in its interrupt, in the order a trace replay takes
them (TRACE.md section 6):

1. At the latch of the screen line COLD names, the power-on reset (§15), then
   the latch. Its raster frame is trace frame 0.
2. For every line after: the latch (`vdp_latch`), the render side caught up, the
   row it starts built and its status published, then the line's records.
3. A read is compared with its value, except where the port selects `STAT5`,
   which is the firmware's version, and in `STAT3`'s bit 1, which is advisory
   (PLAN.md section 4). `/INT` is compared after every operation and at each
   LINE.
4. END records the card's state, and the injection stops applying records. The
   card stays in thread order, frozen, until ABORT.

The raster only paces it: however long a build takes, a read sees what the
replay's read saw. The host streams ahead of the board, and a record that
arrives after its line has passed fails the injection.

---

5. Snapshots
------------

A frame as it went to VGA:

| Field | Size | |
|---|--:|---|
| frame | 4 | the raster frame |
| state matches | 1 | 1 if the state is this frame's |
| state | 165 | below |
| source | 240 | for each row, the row whose buffer its line sent: itself, or an earlier one when late |
| late | 240 | 1 where the row was late |
| rows | 76,800 | 240 rows of 320 palette indices, row-major |

**Rows** are the indices of the buffer each line sent, copied by core 0 in its
line-start interrupt, so a late row appears as the row it repeated. A late row
built before the capture was armed shows indices that were not kept.

**State**, as the card stood when the frame's row 239 had been built: the 128
raw registers (`$02`–`$06` at their homes, §5), each port pair's pointer (2),
prefetch, payload, direction and flip-flop (1 each), the screen line and display
line (2 each), `STAT0`, the latched interrupts, the frame's spent events and
`STAT7` (1 each), the collision map (8), `/INT` (1), the raster frame (4) and the
scene frames begun (4).

---

6. Faults, the Watchdog and Safe Mode
-------------------------------------

**A fault record** is written by a HardFault on either core, or a panic, to RAM
the SDK does not clear at boot (`.uninitialized_data`), sealed with a CRC-32. Then
the board sets watchdog scratch register 0 and reboots through the watchdog.

| Field | Size | |
|---|--:|---|
| kind | 4 | 1 HardFault, 2 panic, 3 watchdog |
| core | 4 | 0, 1, or `$FFFFFFFF` |
| r0–r3, r12, lr, pc, xPSR | 32 | the exception frame |
| EXC_RETURN, SP | 8 | |
| CFSR, HFSR, MMFAR, BFAR, SFSR, SFAR | 24 | |
| stack | 128 | 32 words from the frame |
| uptime | 8 | µs |
| heartbeats | 8 | core 0's thread passes, core 1's lines taken |
| message | 64 | a panic's, or what stopped the watchdog being fed |
| build | 32 | |

`vdpctl info` symbolises PC and LR with `arm-none-eabi-addr2line` against
`build/pico2/firmware/picovdp.elf`.

**The watchdog** is 250 ms, fed by a 50 ms timer on core 0 only while core 1's
renderer has taken a line since the last feed and core 0's thread has passed
within 2 s. When it bites, the next boot writes the record itself, from what the
feeder last saw.

**Safe mode** follows a fault reboot or a watchdog bite: USB up, core 1 never
started, every row a fixed dark red. INFO, REBOOT and FAULT answer; the rest
answer ERROR. A reflash, or REBOOT, boots normally.

**The one manual recovery** is an image that dies before USB enumerates: hold
BOOT and replug. The firmware sets the clock and then starts USB before anything
else.

---

7. Where the Link Runs
----------------------

On core 0, in thread mode, beneath everything else on that core:

| Core 0 | Priority |
|---|---|
| VGA line-start and DMA interrupts | highest |
| core 0's half of each line, posted by core 1 through the inter-core FIFO | `$40` |
| USB, the SDK's timer, the watchdog's feeder | default and below |
| the debug link | thread |

A snapshot's rows are copied in the line-start interrupt, 320 bytes a line
while one is captured. Nothing else the link does can delay a line.

---

8. vdpctl
---------

```sh
node tools/vdpctl.mjs flash [file.uf2]             # picotool load -x -f, then INFO
node tools/vdpctl.mjs info
node tools/vdpctl.mjs stats [--reset] [--json]
node tools/vdpctl.mjs snapshot [--frame N] [--out DIR]
node tools/vdpctl.mjs vram [--out FILE]
node tools/vdpctl.mjs inject <fixture|all|trace> [checkpoint ...] [--out FILE]
node tools/vdpctl.mjs reset [--power-on]
node tools/vdpctl.mjs reboot [--bootsel]
node tools/vdpctl.mjs fault <core0|core1|hang|panic>
node tools/vdpctl.mjs scene <name>
node tools/vdpctl.mjs load [--bus HZ] [--handicap FIRST,LAST,EVERY,CYCLES]
node tools/vdpctl.mjs scene-log
node tools/vdpctl.mjs profile [--row N] [--iterations N] [--json]
node tools/vdpctl.mjs scenes [--seconds N | --minutes N] [--only a,b] [--bus HZ] [--stream] [--profile] [--out FILE]
node tools/vdpctl.mjs late [--scene NAME] [--handicap FIRST,LAST,EVERY,CYCLES] [--seconds N]
```

`scenes` and `late` check each snapshot against the same scene frame drawn by
the core on the host (`build/host/host/scene/vdp-scene`), and `late` compares what
the scene's program read with the host's reads.
