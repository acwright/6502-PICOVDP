// Arduino Mega bus harness — Phase 0 skeleton.
//
// The framed protocol, bus primitives, timing profiles and scripts arrive in
// Phase 9, specified in docs/BENCH.md. Until then this only brings the bus up
// idle and answers on serial, so the build and upload path is proven.
//
// Pin map (PLAN.md section 5): data on PORTA (D22-D29, CD7..CD0 = bit 0..7),
// control on PORTC, /INT on D48 (PL1, ICP5).

#include <Arduino.h>

// PORTC control bits
static const uint8_t CSW = _BV(PC0);   // D37 /CSW
static const uint8_t CSR = _BV(PC1);   // D36 /CSR
static const uint8_t MODE = _BV(PC2);  // D35 MODE = A0
static const uint8_t MODE1 = _BV(PC3); // D34 MODE1 = A1
static const uint8_t RST = _BV(PC4);   // D33 /RESET

static void busIdle() {
  DDRA = 0x00;  // data tri-stated
  PORTA = 0x00; // no pull-ups
  PORTC = (PORTC & ~(MODE | MODE1)) | CSW | CSR | RST; // strobes and /RESET high
  DDRC |= CSW | CSR | MODE | MODE1 | RST;
}

void setup() {
  busIdle();
  Serial.begin(1000000);
  Serial.println(F("picovdp-mega skeleton"));
}

void loop() {
  if (Serial.available()) {
    Serial.read();
    Serial.println(F("picovdp-mega skeleton: no protocol yet (Phase 9)"));
  }
}
