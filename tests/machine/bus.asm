.setcpu "65C02"

; =============================================================================
;   bus — a 6502's tightest accesses, on the AC6502's own bus
; =============================================================================
;   Phase 13 proved accesses 2 us apart with the Nano standing in for the CPU
;   (docs/results/phase-13.md, "2 MHz"). This is the same test made by the
;   machine's W65C02S, whose tightest spacing is 4 cycles: 2 us at 2 MHz, 4 us
;   at 1 MHz. The clock is measured first, against the card's own frame.
;
;   Each pass draws 256 random bytes and, through each port pair in turn,
;
;     writes    4 KB of VRAM at $4000 (VBANK 1, which the console never shows),
;               in pairs 4 cycles apart: `sty VC_DATA : sta VC_DATA`
;     reads     it back in pairs 4 cycles apart: `ldy VC_DATA : lda VC_DATA`,
;               counting a wrong byte as tight (the second of a pair) or loose
;     addresses 1,024 random bytes of it, each read 4 cycles after the address
;               command that names it: `stx VC_REG : lda VC_DATA`
;     status    reads STAT4 2,048 times in pairs 4 cycles apart, every one $AC
;
;   and then copies the 4 KB to $5000 with port B reading and port A writing,
;   `lda VC_DATA2 : sta VC_DATA`, 4 cycles apart, and reads the copy back.
;
;   Interrupts are off throughout: the Kernal's handler owns port A between its
;   own pairs of writes. Counts are in RESULTS for BASIC to PEEK
;   (tools/acectl.mjs reads them); VBANK, VINC and both STATSELs are put back.
; =============================================================================

.include "machine.inc"

; ---- Zero page: $86-$FF is free of BASIC -------------------------------------

RND                 = $F0        ; 2 bytes, xorshift state, never $0000
PAGE                = $F2        ; page of the 4 KB being worked on
PASS                = $FA        ; 2 bytes
CNT                 = $F4        ; 2 bytes
LOW                 = $F6        ; a random address: low byte
EXPECT              = $F7
FRAMES              = $F8

REGION_PAGES        = 16         ; 4 KB
REGION_WRITE        = $40        ; offset $0000 in bank 1 = VRAM $4000, write command
REGION_READ         = $00        ;                                read command
COPY_WRITE          = $50        ; offset $1000 in bank 1 = VRAM $5000, write command
COPY_READ           = $10
CLOCK_FRAMES        = 16

.segment "CODE"

  BasicStub

Start:
  php
  sei
  lda Seed
  sta RND
  lda Seed + 1
  sta RND + 1

  lda #$00                      ; STAT0 on port A, for the clock
  ldx #R_STATSEL_A
  SetRegA
  jsr MeasureClock

  lda #$01                      ; bank 1 for every address command from here
  ldx #R_VBANK
  SetRegA
  lda #$01
  ldx #R_VINC
  SetRegA

  stz PASS
  stz PASS + 1
@Pass:
  jsr NewPattern
  jsr PairA
  jsr PairB
  jsr CopyBA
  inc PASS
  bne :+
  inc PASS + 1
: lda PASS
  cmp Passes
  bne @Pass
  lda PASS + 1
  cmp Passes + 1
  bne @Pass

  lda #$00                      ; the console's bank
  ldx #R_VBANK
  SetRegA
  lda #$A5
  sta Done
  lda #DONE_MARK
  jsr SerialChrout
  plp
  rts

; =============================================================================
;   MeasureClock — CPU cycles in CLOCK_FRAMES of the card's frames
; =============================================================================
;   A loop of exactly 25 cycles counts until STAT0's F has been seen
;   CLOCK_FRAMES times. The frame is 16.6838 ms (docs/results/phase-09.md), so
;   the clock is Clock x 25 / (CLOCK_FRAMES x 16.6838 ms): 21,357 at 2 MHz.

MeasureClock:
  stz CNT
  stz CNT + 1
  lda VC_STATUS                 ; clear F
@Sync:
  bit VC_STATUS
  bpl @Sync                     ; start on a frame's end
  lda #CLOCK_FRAMES
  sta FRAMES
@Loop:
  clc                           ; 2
  lda CNT                       ; 3
  adc #1                        ; 2
  sta CNT                       ; 3
  lda CNT + 1                   ; 3
  adc #0                        ; 2
  sta CNT + 1                   ; 3
  bit VC_STATUS                 ; 4   N = F
  bpl @Loop                     ; 3   taken
  .assert >@Loop = >*, error, "MeasureClock's loop crosses a page: a taken branch would cost 26"
  dec FRAMES
  bne @Loop
  lda CNT
  sta Clock
  lda CNT + 1
  sta Clock + 1
  rts

; =============================================================================
;   The pattern
; =============================================================================
;   VRAM offset p * 256 + i holds Pattern[i] ^ p.

NewPattern:
  ldx #0
@Byte:
  jsr Random
  sta Pattern,x
  inx
  bne @Byte
  rts

; Out: A = a pseudo-random byte. X and Y preserved. (graphics-1.asm's xorshift.)
Random:
  lda RND + 1
  lsr a
  lda RND
  ror a
  eor RND + 1
  sta RND + 1
  ror a
  eor RND
  sta RND
  eor RND + 1
  sta RND + 1
  rts

.macro Bump counter
  .local done
  inc counter
  bne done
  inc counter + 1
  bne done
  inc counter + 2
done:
.endmacro

; Compare Buffer with the pattern's page PAGE; a wrong byte at an odd index is
; the second of a pair, counted in `tight`.
.macro CheckPage tight, loose
  .local next, odd
  ldx #0
: lda Pattern,x
  eor PAGE
  cmp Buffer,x
  beq next
  txa
  lsr a
  bcs odd
  Bump loose
  bra next
odd:
  Bump tight
next:
  inx
  bne :-
.endmacro

; =============================================================================
;   PairTest — one port pair's four tests
; =============================================================================

.macro PairTest DATA, REG, STATUS, STATSEL, ErrTight, ErrLoose, ErrAddr, ErrStat
  .local wPage, w, rPage, r, addr, addrNext, s, s1, s2

  ; ---- Writes, in pairs 4 cycles apart
  lda #$00
  sta REG
  lda #REGION_WRITE
  sta REG
  stz PAGE
wPage:
  ldx #0
w:
  lda Pattern,x
  eor PAGE
  tay
  lda Pattern + 1,x
  eor PAGE
  sty DATA
  sta DATA                      ; 4 cycles after the last
  inx
  inx
  bne w
  inc PAGE
  lda PAGE
  cmp #REGION_PAGES
  bne wPage

  ; ---- Reads, in pairs 4 cycles apart
  lda #$00
  sta REG
  lda #REGION_READ
  sta REG
  stz PAGE
rPage:
  ldx #0
r:
  ldy DATA
  lda DATA                      ; 4 cycles after the last
  sta Buffer + 1,x
  tya
  sta Buffer,x
  inx
  inx
  bne r
  CheckPage ErrTight, ErrLoose
  inc PAGE
  lda PAGE
  cmp #REGION_PAGES
  bne rPage

  ; ---- A read 4 cycles after the address command that names its byte
  lda #<1024
  sta CNT
  lda #>1024
  sta CNT + 1
addr:
  jsr Random
  sta LOW
  jsr Random
  and #REGION_PAGES - 1
  sta PAGE
  ldx LOW
  lda Pattern,x
  eor PAGE
  sta EXPECT
  lda LOW
  ldx PAGE                      ; REGION_READ | page
  sta REG
  stx REG
  lda DATA                      ; 4 cycles after the command
  cmp EXPECT
  beq addrNext
  Bump ErrAddr
addrNext:
  lda CNT
  bne :+
  dec CNT + 1
: dec CNT
  lda CNT
  ora CNT + 1
  bne addr

  ; ---- STAT4, in pairs 4 cycles apart
  lda #$04
  ldx #STATSEL
  SetRegA
  ldx #0
s:
  lda STATUS
  ldy STATUS                    ; 4 cycles after the last
  cmp #$AC
  beq s1
  Bump ErrStat
s1:
  cpy #$AC
  beq s2
  Bump ErrStat
s2:
  inx
  bne s
  lda #$00
  ldx #STATSEL
  SetRegA
  rts
.endmacro

PairA:
  PairTest VC_DATA,  VC_REG,  VC_STATUS,  R_STATSEL_A, ErrTightA, ErrLooseA, ErrAddrA, ErrStatA
PairB:
  PairTest VC_DATA2, VC_REG2, VC_STATUS2, R_STATSEL_B, ErrTightB, ErrLooseB, ErrAddrB, ErrStatB

; =============================================================================
;   CopyBA — port B reads, port A writes, 4 cycles apart; then read it back
; =============================================================================

CopyBA:
  lda #$00
  sta VC_REG2
  lda #REGION_READ
  sta VC_REG2                   ; port B reads from $4000
  lda #$00
  sta VC_REG
  lda #COPY_WRITE
  sta VC_REG                    ; port A writes to $5000
  ldy #REGION_PAGES * 256 / 2048
  ldx #0
@Copy:
  .repeat 8
  lda VC_DATA2
  sta VC_DATA                   ; 4 cycles after the read
  .endrepeat
  dex
  bne @Copy
  dey
  bne @Copy

  lda #$00
  sta VC_REG
  lda #COPY_READ
  sta VC_REG
  stz PAGE
CopyPage:
  ldx #0
CopyRead:
  ldy VC_DATA
  lda VC_DATA
  sta Buffer + 1,x
  tya
  sta Buffer,x
  inx
  inx
  bne CopyRead
  CheckPage ErrCopy, ErrCopy
  inc PAGE
  lda PAGE
  cmp #REGION_PAGES
  bne CopyPage
  rts

; =============================================================================
;   Parameters and results, PEEKed and POKEd by tools/acectl.mjs
; =============================================================================

.segment "RESULTS"

Passes:     .word 16
Seed:       .word $C33C
Clock:      .word 0
ErrTightA:  .res 3
ErrLooseA:  .res 3
ErrAddrA:   .res 3
ErrStatA:   .res 3
ErrTightB:  .res 3
ErrLooseB:  .res 3
ErrAddrB:   .res 3
ErrStatB:   .res 3
ErrCopy:    .res 3
Done:       .byte 0

Pattern:    .res 256
Buffer:     .res 256
