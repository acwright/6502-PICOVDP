// Arduino Nano bus harness — Phase 0 skeleton.
//
// The framed protocol, bus primitives, timing profiles and scripts arrive in
// Phase 9, specified in docs/BENCH.md. Until then this only brings the bus up
// idle and answers on serial, so the build and upload path is proven.
//
// Pin map (PLAN.md section 5): data bits 0-5 on PORTD (D2-D7, CD7..CD2), bits
// 6-7 on PORTB (D9-D10, CD1..CD0), control on PORTC (A0-A4), /INT on D8 (PB0,
// ICP1), optional VSYNC on A5 (PC5, PCINT13).

#include <Arduino.h>

// Data bus
static const uint8_t DATA_D = 0xFC;             // PD2-PD7: data bits 0-5
static const uint8_t DATA_B = _BV(PB1) | _BV(PB2); // PB1-PB2: data bits 6-7

// PORTC control bits
static const uint8_t CSW = _BV(PC0);   // A0 /CSW
static const uint8_t CSR = _BV(PC1);   // A1 /CSR
static const uint8_t MODE = _BV(PC2);  // A2 MODE = CPU A0
static const uint8_t MODE1 = _BV(PC3); // A3 MODE1 = CPU A1
static const uint8_t RST = _BV(PC4);   // A4 /RESET

static void busIdle() {
  DDRD &= ~DATA_D; // data tri-stated, no pull-ups; PD0/PD1 belong to Serial
  PORTD &= ~DATA_D;
  DDRB &= ~DATA_B;
  PORTB &= ~DATA_B;
  PORTC = (PORTC & ~(MODE | MODE1)) | CSW | CSR | RST; // strobes and /RESET high
  DDRC |= CSW | CSR | MODE | MODE1 | RST;
}

void setup() {
  busIdle();
  Serial.begin(1000000);
  Serial.println(F("picovdp-nano skeleton"));
}

void loop() {
  if (Serial.available()) {
    Serial.read();
    Serial.println(F("picovdp-nano skeleton: no protocol yet (Phase 9)"));
  }
}
