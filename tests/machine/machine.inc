; What the Phase 14 programs share: the AC6502's ports, and the BASIC stub that
; makes a .prg (prg.cfg). Addresses are BIOS 2.0's (6502-BIOS BIOS.inc).

VC_DATA             := $9C00     ; port A: VRAM data (§4)
VC_REG              := $9C01     ; port A: command
VC_STATUS           := $9C01     ; port A: status
VC_DATA2            := $9C02     ; port B
VC_REG2             := $9C03
VC_STATUS2          := $9C03

SC_STATUS           := $9001     ; the R6551's status register
SC_RDRF             = %00001000  ; a byte has arrived

R_VBANK             = $08
R_VINC              = $09
R_STATSEL_B         = $0E
R_STATSEL_A         = $0F

RESET_VECTOR        := $FFFC
SerialChrout        := $A051     ; a byte to the serial port, whatever IO_MODE says

; What a program sends when it has finished, so the host need not type at a
; machine that has interrupts off and would drop the keys.
DONE_MARK           = '!'

; `10 SYS 2060`, tokenized: the first twelve bytes of every .prg, so that RUN
; lands on the byte after it, $080C.
.macro BasicStub
  .byte $0A, $08, $0A, $00, $A5, "2060", $00, $00, $00
.endmacro

; Write A into register X through port A.
.macro SetRegA
  sta VC_REG
  txa
  ora #$80
  sta VC_REG
.endmacro
