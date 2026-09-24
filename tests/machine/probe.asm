.setcpu "65C02"

; =============================================================================
;   probe — §16's detection, run by a 6502 on the AC6502's bus
; =============================================================================
;   PLAN.md Phase 14: "§16's detection probe returns carry set." DetectVdp and
;   DetectFont are SPEC.md §16's listings, copied as they stand, and each is run
;   RUNS times with interrupts off. The counts of carry set, and one plain read
;   of STAT4-STAT6 through each port pair, are left in RESULTS for BASIC to PEEK
;   (tools/acectl.mjs reads them).
; =============================================================================

.include "machine.inc"

RUNS                = 10000

COUNT               = $F0        ; 2 bytes, counting down ($86-$FF: free of BASIC)

.segment "CODE"

  BasicStub

Start:
  php
  sei
  lda #<RUNS
  sta COUNT
  lda #>RUNS
  sta COUNT + 1
@Run:
  jsr DetectVdp
  bcc @NoVdp
  inc VdpFound
  bne @Font
  inc VdpFound + 1
@Font:
  jsr DetectFont
  bcc @Next
  inc FontFound
  bne @Next
  inc FontFound + 1
  bra @Next
@NoVdp:
@Next:
  lda COUNT
  bne :+
  dec COUNT + 1
: dec COUNT
  lda COUNT
  ora COUNT + 1
  bne @Run

  ; STAT4-STAT6, once each, through port A and then port B.
  ldy #0
@StatA:
  tya
  clc
  adc #4
  ldx #R_STATSEL_A
  SetRegA
  lda VC_STATUS
  sta StatA,y
  iny
  cpy #3
  bne @StatA
  lda #$00                      ; STAT0 back on port A
  ldx #R_STATSEL_A
  SetRegA

  ldy #0
@StatB:
  tya
  clc
  adc #4
  ldx #R_STATSEL_B
  SetRegA                       ; a register write, from either port
  lda VC_STATUS2
  sta StatB,y
  iny
  cpy #3
  bne @StatB
  lda #$00
  ldx #R_STATSEL_B
  SetRegA

  lda #$A5                      ; the program ran to its end
  sta Done
  lda #DONE_MARK
  jsr SerialChrout
  plp
  rts

; ---- SPEC.md §16, verbatim ---------------------------------------------------

; Returns carry set if a 6502-PICOVDP is fitted
DetectVdp:
  lda VC_STATUS                 ; resynchronize the command flip-flop (§4)
  lda #$04                      ; select STAT4
  sta VC_REG
  lda #$8F                      ; register $0F | $80
  sta VC_REG
  lda VC_STATUS
  cmp #$AC
  beq @found
  clc
  rts
@found:
  lda #$00                      ; put STAT0 back
  sta VC_REG
  lda #$8F
  sta VC_REG
  sec
  rts

; After DetectVdp returns carry set: carry set if the card has the built-in font
DetectFont:
  lda #$06                      ; select STAT6
  sta VC_REG
  lda #$8F                      ; register $0F | $80
  sta VC_REG
  lda VC_STATUS
  asl                           ; b7 into carry
  lda #$00                      ; put STAT0 back; lda and sta leave carry alone
  sta VC_REG
  lda #$8F
  sta VC_REG
  rts

; ------------------------------------------------------------------------------

.segment "RESULTS"

Runs:       .word RUNS
VdpFound:   .word 0
FontFound:  .word 0
StatA:      .res 3
StatB:      .res 3
Done:       .byte 0

.export Runs, VdpFound, FontFound, StatA, StatB, Done
