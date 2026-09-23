// Arduino Nano bus harness for the PICO9918 PRO.
//
// A bus-script runner, not a byte relay: anything timing-critical happens here,
// because USB serial latency is milliseconds. The wire format and the command
// set are specified in docs/BENCH.md section 5.
//
// Pin map (PLAN.md section 5, bench/hardware/picovdp-bench.kicad_sch):
//   data bits 0-5  PD2-PD7  (D2-D7)   = CD7..CD2   -- CD7 is the LSB
//   data bits 6-7  PB1-PB2  (D9-D10)  = CD1, CD0   -- CD0 is the MSB
//   control        PC0-PC4  (A0-A4)   = /CSW /CSR MODE MODE1 /RESET
//   /INT           PB0      (D8)      = ICP1, Timer 1 input capture
//   frame sync     PC5      (A5)      = PCINT13, optional, Phase 13

#include <Arduino.h>
#include <util/delay_basic.h>

static const uint8_t FIRMWARE_VERSION = 1;
static const uint8_t PROTOCOL_VERSION = 1;

// ---------------------------------------------------------------- the bus

static const uint8_t DATA_D = 0xFC;                  // PD2-PD7
static const uint8_t DATA_B = _BV(PB1) | _BV(PB2);   // PB1-PB2

static const uint8_t CSW = _BV(PC0);
static const uint8_t CSR = _BV(PC1);
static const uint8_t MODE = _BV(PC2);
static const uint8_t MODE1 = _BV(PC3);
static const uint8_t RST = _BV(PC4);
static const uint8_t CTRL = CSW | CSR | MODE | MODE1 | RST;
static const uint8_t IDLE_C = CSW | CSR | RST;       // strobes and /RESET high

// Timing, in _delay_loop_1 counts (3 cycles = 187.5 ns at 16 MHz) except the
// inter-access gap, which is microseconds. Defaults are the 6502-1mhz profile.
//
// Everything on the strobe's critical path is force-inlined and its operands
// are in registers before the strobe falls. Left to itself the compiler emits a
// call for spin() and an lds for the count, which put 14 cycles -- 875 ns --
// between the two PORTC writes even with the count at zero. That is wider than
// a 1 MHz 6502's strobe, so no sweep could ever find an edge.
#define HOT __attribute__((always_inline)) inline

static uint8_t tSetup = 2, tWidth = 2, tHold = 2, tGap = 4;

static HOT void spin(uint8_t n) {
  if (n) _delay_loop_1(n);
}

static inline void gap() {
  if (tGap) _delay_loop_2((uint16_t)tGap * 4);       // 4 cycles per count
}

// Drive the data bus. The port latches are set before the drivers are enabled,
// so no stale value ever reaches the pins.
static HOT void dataDrive(uint8_t v) {
  PORTD = (PORTD & ~DATA_D) | ((v << 2) & DATA_D);
  PORTB = (PORTB & ~DATA_B) | ((v >> 5) & DATA_B);
  DDRD |= DATA_D;
  DDRB |= DATA_B;
}

// Release it. The drivers are disabled before the pull-ups are cleared.
static HOT void dataRelease() {
  DDRD &= ~DATA_D;
  DDRB &= ~DATA_B;
  PORTD &= ~DATA_D;
  PORTB &= ~DATA_B;
}

static HOT uint8_t dataSample() {
  uint8_t d = PIND;                                  // one instruction apart;
  uint8_t b = PINB;                                  // the PRO holds the bus
  return ((d & DATA_D) >> 2) | ((b & DATA_B) << 5);
}

static void busIdle() {
  dataRelease();
  PORTC = (PORTC & ~CTRL) | IDLE_C;                  // latch first,
  DDRC |= CTRL;                                      // then drive
}

// Only ever one strobe is lowered, so /CSR and /CSW can never be low together.
// /CSW is low for 3 x width + 1 cycles: two back-to-back `out`s when width is 0.
static void busWrite(uint8_t port, uint8_t value) {
  const uint8_t c = IDLE_C | ((port & 3) << 2);
  const uint8_t low = c & (uint8_t)~CSW;
  const uint8_t width = tWidth;                      // in a register before the strobe
  dataDrive(value);
  PORTC = c;
  spin(tSetup);
  if (width) {
    PORTC = low;
    _delay_loop_1(width);
    PORTC = c;
  } else {
    PORTC = low;
    PORTC = c;
  }
  spin(tHold);
  dataRelease();
  gap();
}

// /CSR is low for 3 x width + 3 cycles: the two samples and the closing `out`.
static uint8_t busRead(uint8_t port) {
  const uint8_t c = IDLE_C | ((port & 3) << 2);
  const uint8_t low = c & (uint8_t)~CSR;
  const uint8_t width = tWidth;
  uint8_t v;
  dataRelease();                                     // tri-stated before /CSR falls
  PORTC = c;
  spin(tSetup);
  if (width) {
    PORTC = low;
    _delay_loop_1(width);
    v = dataSample();
    PORTC = c;
  } else {
    PORTC = low;
    v = dataSample();
    PORTC = c;
  }
  spin(tHold);
  gap();
  return v;
}

static void busReset(uint16_t microseconds) {
  PORTC = IDLE_C & (uint8_t)~RST;
  while (microseconds--) _delay_loop_2(4);
  PORTC = IDLE_C;
  gap();
}

// ---------------------------------------------------------------- /INT capture

static volatile uint16_t t1High = 0;
static volatile uint32_t intLast = 0;
static volatile uint16_t intEdges = 0;

ISR(TIMER1_OVF_vect) {
  t1High++;
}

ISR(TIMER1_CAPT_vect) {
  const uint16_t low = ICR1;
  uint16_t high = t1High;
  // An overflow pending with a low capture means the overflow followed it.
  if ((TIFR1 & _BV(TOV1)) && low < 0x8000) high++;
  intLast = ((uint32_t)high << 16) | low;
  intEdges++;
}

static void captureBegin() {
  TCCR1A = 0;
  TCCR1B = _BV(ICNC1) | _BV(CS10);   // noise canceller, /1, falling edge
  TIMSK1 = _BV(ICIE1) | _BV(TOIE1);
}

// ---------------------------------------------------------------- framing

static const uint8_t ERROR = 0xFF;
static const uint8_t ANSWER = 0x80;

enum : uint8_t {
  CMD_PING = 0x01,
  CMD_PROFILE = 0x02,
  CMD_WRITE = 0x03,
  CMD_READ = 0x04,
  CMD_WRITE_BLOCK = 0x05,
  CMD_READ_BLOCK = 0x06,
  CMD_RESET = 0x07,
  CMD_IDLE = 0x08,
  CMD_INT = 0x09,
  CMD_INT_PERIOD = 0x0A,
};

enum : uint8_t {
  ERR_CRC = 1,
  ERR_UNKNOWN = 2,
  ERR_LENGTH = 3,
  ERR_TIMEOUT = 4,
};

// The receive ring is 256 bytes (platformio.ini). Raising it is not an option:
// over 256 the Arduino core switches its ring indices to uint16_t and serial
// stops working on this part. So the largest packet has to fit inside it --
// 5 header + payload + 2 CRC -- and 240 data bytes leaves comfortable room.
// Bigger blocks worked in isolation and desynced under sustained load.
static const uint8_t BLOCK_MAX = 240;

static uint8_t body[258];

static uint16_t crcByte(uint16_t crc, uint8_t b) {
  crc ^= (uint16_t)b << 8;
  for (uint8_t i = 0; i < 8; i++)
    crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  return crc;
}

static void reply(uint8_t type, uint8_t sequence, const uint8_t *payload, uint8_t length) {
  uint8_t head[5] = { 'N', 'B', type, sequence, length };
  uint16_t crc = 0xFFFF;
  for (uint8_t i = 2; i < 5; i++) crc = crcByte(crc, head[i]);
  for (uint8_t i = 0; i < length; i++) crc = crcByte(crc, payload[i]);
  Serial.write(head, 5);
  if (length) Serial.write(payload, length);
  const uint8_t tail[2] = { (uint8_t)(crc & 0xFF), (uint8_t)(crc >> 8) };
  Serial.write(tail, 2);
}

static void fail(uint8_t sequence, uint8_t code) {
  reply(ERROR, sequence, &code, 1);
}

static bool take(uint8_t *destination, uint16_t count, uint16_t milliseconds) {
  uint32_t last = millis();
  uint16_t got = 0;
  while (got < count) {
    if (Serial.available()) {
      destination[got++] = (uint8_t)Serial.read();
      last = millis();
    } else if (millis() - last > milliseconds) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------- commands

static uint32_t intTicks() {
  uint32_t t;
  const uint8_t sreg = SREG;
  cli();
  t = intLast;
  SREG = sreg;
  return t;
}

static void dispatch(uint8_t type, uint8_t sequence, uint8_t *payload, uint8_t length) {
  switch (type) {
    case CMD_PING: {
      const uint8_t out[2] = { FIRMWARE_VERSION, PROTOCOL_VERSION };
      reply(type | ANSWER, sequence, out, 2);
      return;
    }
    case CMD_PROFILE: {
      if (length != 4) { fail(sequence, ERR_LENGTH); return; }
      tSetup = payload[0];
      tWidth = payload[1];
      tHold = payload[2];
      tGap = payload[3];
      reply(type | ANSWER, sequence, nullptr, 0);
      return;
    }
    case CMD_WRITE: {
      if (length != 2) { fail(sequence, ERR_LENGTH); return; }
      busWrite(payload[0], payload[1]);
      reply(type | ANSWER, sequence, nullptr, 0);
      return;
    }
    case CMD_READ: {
      if (length != 1) { fail(sequence, ERR_LENGTH); return; }
      const uint8_t v = busRead(payload[0]);
      reply(type | ANSWER, sequence, &v, 1);
      return;
    }
    case CMD_WRITE_BLOCK: {
      if (length < 1 || length > BLOCK_MAX + 1) { fail(sequence, ERR_LENGTH); return; }
      const uint8_t port = payload[0];
      for (uint8_t i = 1; i < length; i++) busWrite(port, payload[i]);
      reply(type | ANSWER, sequence, nullptr, 0);
      return;
    }
    case CMD_READ_BLOCK: {
      if (length != 2) { fail(sequence, ERR_LENGTH); return; }
      const uint8_t port = payload[0];
      const uint8_t count = payload[1];
      if (count == 0 || count > BLOCK_MAX) { fail(sequence, ERR_LENGTH); return; }
      for (uint8_t i = 0; i < count; i++) body[i] = busRead(port);
      reply(type | ANSWER, sequence, body, count);
      return;
    }
    case CMD_RESET: {
      if (length != 2) { fail(sequence, ERR_LENGTH); return; }
      busReset((uint16_t)payload[0] | ((uint16_t)payload[1] << 8));
      reply(type | ANSWER, sequence, nullptr, 0);
      return;
    }
    case CMD_IDLE: {
      busIdle();
      reply(type | ANSWER, sequence, nullptr, 0);
      return;
    }
    case CMD_INT: {
      uint32_t last;
      uint16_t edges;
      uint8_t sreg = SREG;
      cli();
      last = intLast;
      edges = intEdges;
      SREG = sreg;
      const uint8_t out[7] = {
        (uint8_t)((PINB & _BV(PB0)) ? 1 : 0),
        (uint8_t)(edges & 0xFF), (uint8_t)(edges >> 8),
        (uint8_t)(last & 0xFF), (uint8_t)(last >> 8),
        (uint8_t)(last >> 16), (uint8_t)(last >> 24),
      };
      reply(type | ANSWER, sequence, out, 7);
      return;
    }
    case CMD_INT_PERIOD: {
      // Catch `count` falling edges of /INT, reading the status port after each
      // so the card can assert it again. Doing this on the Nano is the point:
      // a host round trip is milliseconds against a 16.68 ms frame, so from
      // there some frames would be missed and the average would come out a
      // multiple of the period rather than the period.
      if (length != 2) { fail(sequence, ERR_LENGTH); return; }
      const uint8_t count = payload[0];
      const uint16_t budget = (uint16_t)payload[1] * 100;   // hundredths of a second
      if (count < 2) { fail(sequence, ERR_LENGTH); return; }
      const uint32_t deadline = millis() + budget;
      uint32_t first = 0, last = 0;
      uint8_t got = 0;
      busRead(1);                                            // clear anything pending
      while (got < count) {
        const uint16_t before = intEdges;
        while (intEdges == before) {
          if ((int32_t)(millis() - deadline) >= 0) goto done;
        }
        const uint32_t t = intTicks();
        if (got == 0) first = t;
        last = t;
        got++;
        busRead(1);                                          // let /INT rise, then fall again
      }
    done: {
        const uint8_t out[9] = {
          got,
          (uint8_t)(first), (uint8_t)(first >> 8), (uint8_t)(first >> 16), (uint8_t)(first >> 24),
          (uint8_t)(last), (uint8_t)(last >> 8), (uint8_t)(last >> 16), (uint8_t)(last >> 24),
        };
        reply(type | ANSWER, sequence, out, 9);
      }
      return;
    }
    default:
      fail(sequence, ERR_UNKNOWN);
      return;
  }
}

// ---------------------------------------------------------------- main

void setup() {
  busIdle();
  captureBegin();
  Serial.begin(1000000);
}

void loop() {
  if (!Serial.available()) return;
  if ((uint8_t)Serial.read() != 'N') return;         // hunt for the sync word

  uint8_t b;
  if (!take(&b, 1, 50) || b != 'B') return;

  uint8_t head[3];
  if (!take(head, 3, 50)) return;
  const uint8_t type = head[0], sequence = head[1], length = head[2];

  if (!take(body, (uint16_t)length + 2, 200)) { fail(sequence, ERR_TIMEOUT); return; }

  uint16_t crc = 0xFFFF;
  for (uint8_t i = 0; i < 3; i++) crc = crcByte(crc, head[i]);
  for (uint8_t i = 0; i < length; i++) crc = crcByte(crc, body[i]);
  const uint16_t want = (uint16_t)body[length] | ((uint16_t)body[length + 1] << 8);
  if (crc != want) { fail(sequence, ERR_CRC); return; }

  dispatch(type, sequence, body, length);
}
