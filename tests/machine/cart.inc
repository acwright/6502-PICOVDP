; A sample cartridge from 6502-EMULATOR, run from RAM as a .prg.
;
; The AC6502 on the bench has no way to put a .crt in its slot, and these two
; are self-contained: 1 KB of code at $C000 that calls only KernalInit and
; keeps interrupts off. So the wrapper includes the cartridge's own source,
; unchanged, straight after the BASIC stub, and RUN enters at CartReset as a
; cold reset would. The vectors it declares land in RAM after it, unread.
;
; The one thing added is a way out. Both cartridges loop forever, so the
; wrapper routes the first `jsr` at Main through Leave, which checks the
; serial port each time round: a byte arriving there soft-resets the machine
; through the reset vector, back to BASIC, with no hand on the reset button.
; It costs a dozen cycles a frame and touches nothing the picture depends on.
;
; Use: .include "cart.inc", then .include the cartridge's source. The names
; here are the wrapper's own, because the cartridge declares VC_DATA, R_VBANK
; and the rest itself.

CART_SC_STATUS      := $9001     ; the R6551's status register
CART_SC_RDRF        = %00001000  ; a byte has arrived
CART_RESET_VECTOR   := $FFFC

.setcpu "65C02"

.segment "CODE"

  .byte $0A, $08, $0A, $00, $A5, "2060", $00, $00, $00     ; 10 SYS 2060

Entry:
  lda Main                      ; `jsr` — anything else is left alone
  cmp #$20
  bne @Go
  lda Main + 1
  sta Resume + 1
  lda Main + 2
  sta Resume + 2
  lda #<Leave
  sta Main + 1
  lda #>Leave
  sta Main + 2
@Go:
  jmp CartReset

Leave:
  lda CART_SC_STATUS
  and #CART_SC_RDRF
  beq Resume
  jmp (CART_RESET_VECTOR)
Resume:
  jmp $0000                     ; Main's own target, filled in above
