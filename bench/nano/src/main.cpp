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

static const uint8_t FIRMWARE_VERSION = 3;
static const uint8_t PROTOCOL_VERSION = 3;

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

// Timer 1's count, extended by its overflows: 62.5 ns a tick.
static uint32_t ticksNow() {
  const uint8_t sreg = SREG;
  cli();
  const uint16_t low = TCNT1;
  uint16_t high = t1High;
  if ((TIFR1 & _BV(TOV1)) && low < 0x8000) high++;
  SREG = sreg;
  return ((uint32_t)high << 16) | low;
}

static void captureBegin() {
  TCCR1A = 0;
  TCCR1B = _BV(ICNC1) | _BV(CS10);   // noise canceller, /1, falling edge
  TIMSK1 = _BV(ICIE1) | _BV(TOIE1);
}

// ---------------------------------------------------------------- scripts
//
// SCRIPT's loop (Phase 11), written to go as fast as a 6502 does: a 1 MHz part
// reads back to back every 4 us, 64 cycles here, and a 2 MHz one every 2 us.
// The strobes are the same instruction sequences busWrite and busRead compile
// to, in assembly so they stay that way, so the profiles' widths and sample
// points are Phase 9's (docs/BENCH.md). Around them, nothing is reloaded or
// called: the timings are in registers, the port arrives already in PORTC's
// bits, the data drivers stay on from one write to the next and are released
// before a read, and /CSR rises as soon as the data is sampled. The profile's
// gap is the period from one access's strobe to the next, paced by Timer 1, as
// a CPU's instruction timing spaces them; with no gap the loop runs flat out.

static const uint8_t SCRIPT_LOG = 8;

// /CSW low for 3 x width + 2 cycles, 4 at width 0: busWrite's strobe.
static HOT void strobeWrite(uint8_t c, uint8_t low, uint8_t width) {
  asm volatile(
    "out %[port], %[low]\n\t"
    "tst %[w]\n\t"
    "breq 2f\n\t"
    "1: dec %[w]\n\t"
    "brne 1b\n\t"
    "2: out %[port], %[c]\n\t"
    : [w] "+r"(width)
    : [port] "I"(_SFR_IO_ADDR(PORTC)), [low] "r"(low), [c] "r"(c));
}

// The same at width 0 with nothing to test: /CSW low for 4 cycles.
static HOT void strobeWriteFlat(uint8_t c, uint8_t low) {
  asm volatile(
    "out %[port], %[low]\n\t"
    "rjmp .+0\n\t"
    "nop\n\t"
    "out %[port], %[c]\n\t"
    :
    : [port] "I"(_SFR_IO_ADDR(PORTC)), [low] "r"(low), [c] "r"(c));
}

// The data sampled 3 cycles after /CSR falls at width 0, as busRead samples it,
// and /CSR raised straight after.
static HOT uint8_t strobeRead(uint8_t c, uint8_t low, uint8_t width) {
  uint8_t d, b;
  asm volatile(
    "out %[port], %[low]\n\t"
    "tst %[w]\n\t"
    "breq 2f\n\t"
    "1: dec %[w]\n\t"
    "brne 1b\n\t"
    "2: in %[d], %[pind]\n\t"
    "in %[b], %[pinb]\n\t"
    "out %[port], %[c]\n\t"
    : [w] "+r"(width), [d] "=&r"(d), [b] "=&r"(b)
    : [port] "I"(_SFR_IO_ADDR(PORTC)), [low] "r"(low), [c] "r"(c),
      [pind] "I"(_SFR_IO_ADDR(PIND)), [pinb] "I"(_SFR_IO_ADDR(PINB)));
  return (uint8_t)((d & DATA_D) >> 2) | (uint8_t)(__builtin_avr_swap((uint8_t)(b & DATA_B)) << 1);
}

// The same at width 0 with nothing to test: sampled 3 cycles after the fall.
static HOT uint8_t strobeReadFlat(uint8_t c, uint8_t low) {
  uint8_t d, b;
  asm volatile(
    "out %[port], %[low]\n\t"
    "rjmp .+0\n\t"
    "nop\n\t"
    "in %[d], %[pind]\n\t"
    "in %[b], %[pinb]\n\t"
    "out %[port], %[c]\n\t"
    : [d] "=&r"(d), [b] "=&r"(b)
    : [port] "I"(_SFR_IO_ADDR(PORTC)), [low] "r"(low), [c] "r"(c),
      [pind] "I"(_SFR_IO_ADDR(PIND)), [pinb] "I"(_SFR_IO_ADDR(PINB)));
  return (uint8_t)((d & DATA_D) >> 2) | (uint8_t)(__builtin_avr_swap((uint8_t)(b & DATA_B)) << 1);
}

static void waitMicroseconds(uint8_t n) {
  while (n--) _delay_loop_2(4);
}

struct ScriptLog {
  const uint8_t *start;
  uint8_t *entries;
  uint8_t differed;
};

// A read that differed: off the loop's path.
static __attribute__((noinline)) void scriptMiss(ScriptLog &log, const uint8_t *p, uint8_t value, uint8_t got) {
  if (log.differed < SCRIPT_LOG) {
    uint8_t *e = log.entries + 3 * log.differed;
    e[0] = (uint8_t)((p - log.start) / 2 - 1);
    e[1] = value;
    e[2] = got;
  }
  if (log.differed < 255) log.differed++;
}

static __attribute__((noinline)) void scriptControl(uint8_t op, uint8_t value) {
  if (op & 0x0C) {
    waitMicroseconds(value);
  } else {
    PORTC = IDLE_C & (uint8_t)~RST;
    waitMicroseconds(value);
    PORTC = IDLE_C;
  }
}

// kFlat: the fastest profile, no setup, hold or gap and width 0, so nothing is
// tested between the strobes' edges.
template <bool kFlat>
static uint8_t runScript(const uint8_t *p, uint8_t count, ScriptLog &log) {
  const uint8_t setup = tSetup, width = tWidth, hold = tHold;
  const uint16_t period = (uint16_t)tGap * 16;
  uint16_t last = TCNT1 - period;
  bool driving = false;
  const uint16_t *w = (const uint16_t *)p;  // op and value in one load: the AVR has no alignment
  for (; count; count--) {
    const uint16_t pair = *w++;
    const uint8_t op = (uint8_t)pair;
    const uint8_t value = (uint8_t)(pair >> 8);
    const uint8_t c = (uint8_t)((op & 0x0C) | IDLE_C);
    if (!(op & 0x80)) {
      if (!(op & 0x40)) {
        // A write.
        PORTD = (uint8_t)((PORTD & (uint8_t)~DATA_D) | (uint8_t)(value << 2));
        PORTB = (uint8_t)((PORTB & (uint8_t)~DATA_B) | ((uint8_t)(__builtin_avr_swap(value) >> 1) & DATA_B));
        if (!driving) {
          DDRD |= DATA_D;
          DDRB |= DATA_B;
          driving = true;
        }
        if (!kFlat && period) {
          while ((uint16_t)(TCNT1 - last) < period) {
          }
          last += period;
        }
        PORTC = c;
        if (kFlat) {
          strobeWriteFlat(c, (uint8_t)(c & ~CSW));
        } else {
          spin(setup);
          strobeWrite(c, (uint8_t)(c & ~CSW), width);
          spin(hold);
        }
        continue;
      }
    } else if (op & 0x40) {
      scriptControl(op, value);
      continue;
    }
    // A read, compared (b7 clear) or not.
    if (driving) {
      dataRelease();
      driving = false;
    }
    if (!kFlat && period) {
      while ((uint16_t)(TCNT1 - last) < period) {
      }
      last += period;
    }
    PORTC = c;
    uint8_t got;
    if (kFlat) {
      got = strobeReadFlat(c, (uint8_t)(c & ~CSR));
    } else {
      spin(setup);
      got = strobeRead(c, (uint8_t)(c & ~CSR), width);
      spin(hold);
    }
    if (!(op & 0x80) && got != value) scriptMiss(log, (const uint8_t *)w, value, got);
  }
  if (driving) dataRelease();
  PORTC = IDLE_C;
  return log.differed;
}

static uint8_t runScript(const uint8_t *p, uint8_t length, uint8_t *entries) {
  ScriptLog log = {p, entries, 0};
  if (!tSetup && !tWidth && !tHold && !tGap) return runScript<true>(p, length / 2, log);
  return runScript<false>(p, length / 2, log);
}

// Back-to-back reads of one port at a set spacing (Phase 11): the prefetch
// restaged between one read and the next is the bus's tightest case (PLAN.md
// risk 5). Each iteration is 18 cycles with `extra` 0 and 16 + 3 x extra
// otherwise: 1.125 us flat out, 2 us at 5 or 6, 4 us exactly at 16. /CSR is low
// for 5 cycles and sampled 3 cycles after it falls, as SCRIPT's flat reads are.
// `raw` takes PIND and PINB for each read, combined after the run. Returns
// Timer 1's ticks for the reads alone.
static uint32_t readRun(uint8_t port, uint8_t count, uint8_t extra, uint8_t *raw) {
  const uint8_t c = (uint8_t)(IDLE_C | ((port & 3) << 2));
  const uint8_t low = (uint8_t)(c & ~CSR);
  dataRelease();
  PORTC = c;
  uint8_t *x = raw;
  uint8_t n = count;
  uint8_t d, b, k;
  const uint32_t began = ticksNow();
  asm volatile(
    "1: out %[portc], %[low]\n\t"
    "rjmp .+0\n\t"
    "nop\n\t"
    "in %[d], %[pind]\n\t"
    "in %[b], %[pinb]\n\t"
    "out %[portc], %[c]\n\t"
    "st %a[x]+, %[d]\n\t"
    "st %a[x]+, %[b]\n\t"
    "mov %[k], %[extra]\n\t"
    "tst %[k]\n\t"
    "breq 3f\n\t"
    "2: dec %[k]\n\t"
    "brne 2b\n\t"
    "3: dec %[n]\n\t"
    "brne 1b\n\t"
    : [x] "+e"(x), [n] "+r"(n), [d] "=&r"(d), [b] "=&r"(b), [k] "=&r"(k)
    : [portc] "I"(_SFR_IO_ADDR(PORTC)), [low] "r"(low), [c] "r"(c), [extra] "r"(extra),
      [pind] "I"(_SFR_IO_ADDR(PIND)), [pinb] "I"(_SFR_IO_ADDR(PINB))
    : "memory");
  const uint32_t spent = ticksNow() - began;
  for (uint8_t i = 0; i < count; i++) {
    raw[i] = (uint8_t)((raw[2 * i] & DATA_D) >> 2) | (uint8_t)(__builtin_avr_swap((uint8_t)(raw[2 * i + 1] & DATA_B)) << 1);
  }
  return spent;
}

// ---------------------------------------------------------------- paced runs
//
// Phase 13: accesses at an exact spacing, whatever their mix — a data read
// straight after the address command that sets it, writes back to back, a
// scanline handler's writes — as a CPU's instruction timing spaces them. Each
// access is precomputed into the six port values it needs, so the loop below
// does the same instructions for a read as for a write: 32 cycles an access,
// 2 us, with `extra` 0, and 31 + 3 x extra cycles otherwise (4 us at 11). The
// strobe is READ_RUN's: low for 6 cycles, the data sampled 3 cycles after it
// falls, so a read's /CSR rises 1.625 us before the next access's strobe
// falls. Interrupts are off while it runs, so nothing stretches a gap.

static const uint8_t PACED_MAX = 100;

struct Entry {
  uint8_t pd, pb, dd, db, ch, cl;  // PORTD, PORTB, DDRD, DDRB, PORTC idle and strobed
};

static Entry entries[PACED_MAX];
// READ_RUN's samples, BLOCK_MAX + 4 bytes; a paced run's, two a access, and
// the scanline handler's after them.
static uint8_t scratch[244];

// PACED's, INT_RUN's and TRAFFIC's answers, built here rather than on a stack
// the RAM has little room for.
static uint8_t answer[255];

// An op as SCRIPT has it: b7:6 0 write, 1 read and compare, 2 read and
// ignore; b3:2 the port.
static void prepare(Entry &e, uint8_t op, uint8_t value) {
  const uint8_t c = (uint8_t)((op & 0x0C) | IDLE_C);
  const uint8_t d = (uint8_t)(PORTD & ~DATA_D), b = (uint8_t)(PORTB & ~DATA_B);
  e.ch = c;
  if (!(op & 0xC0)) {
    e.pd = (uint8_t)(d | ((uint8_t)(value << 2) & DATA_D));
    e.pb = (uint8_t)(b | ((uint8_t)(__builtin_avr_swap(value) >> 1) & DATA_B));
    e.dd = (uint8_t)(DDRD | DATA_D);
    e.db = (uint8_t)(DDRB | DATA_B);
    e.cl = (uint8_t)(c & ~CSW);
  } else {
    e.pd = d;                                          // no pull-ups on the bus
    e.pb = b;
    e.dd = (uint8_t)(DDRD & ~DATA_D);
    e.db = (uint8_t)(DDRB & ~DATA_B);
    e.cl = (uint8_t)(c & ~CSR);
  }
}

static inline uint8_t sampled(const uint8_t *raw) {
  return (uint8_t)((raw[0] & DATA_D) >> 2) | (uint8_t)(__builtin_avr_swap((uint8_t)(raw[1] & DATA_B)) << 1);
}

// `count` entries at the spacing above, PIND and PINB for each into `raw`.
// Interrupts must be off.
static void pacedRun(const Entry *e, uint8_t count, uint8_t extra, uint8_t *raw) {
  uint8_t pd, pb, dd, db, ch, cl, d, b, k;
  const Entry *x = e;
  uint8_t *z = raw;
  uint8_t n = count;
  if (!extra) {
    asm volatile(
      "1: ld %[pd], X+\n\t"
      "ld %[pb], X+\n\t"
      "ld %[dd], X+\n\t"
      "ld %[db], X+\n\t"
      "ld %[ch], X+\n\t"
      "ld %[cl], X+\n\t"
      "out %[portd], %[pd]\n\t"
      "out %[portb], %[pb]\n\t"
      "out %[ddrd], %[dd]\n\t"
      "out %[ddrb], %[db]\n\t"
      "out %[portc], %[ch]\n\t"
      "out %[portc], %[cl]\n\t"
      "rjmp .+0\n\t"
      "nop\n\t"
      "in %[d], %[pind]\n\t"
      "in %[b], %[pinb]\n\t"
      "out %[portc], %[ch]\n\t"
      "st Z+, %[d]\n\t"
      "st Z+, %[b]\n\t"
      "nop\n\t"
      "dec %[n]\n\t"
      "brne 1b\n\t"
      : [x] "+x"(x), [z] "+z"(z), [n] "+r"(n), [pd] "=&r"(pd), [pb] "=&r"(pb), [dd] "=&r"(dd), [db] "=&r"(db),
        [ch] "=&r"(ch), [cl] "=&r"(cl), [d] "=&r"(d), [b] "=&r"(b)
      : [portd] "I"(_SFR_IO_ADDR(PORTD)), [portb] "I"(_SFR_IO_ADDR(PORTB)), [ddrd] "I"(_SFR_IO_ADDR(DDRD)),
        [ddrb] "I"(_SFR_IO_ADDR(DDRB)), [portc] "I"(_SFR_IO_ADDR(PORTC)), [pind] "I"(_SFR_IO_ADDR(PIND)),
        [pinb] "I"(_SFR_IO_ADDR(PINB))
      : "memory");
  } else {
    asm volatile(
      "1: ld %[pd], X+\n\t"
      "ld %[pb], X+\n\t"
      "ld %[dd], X+\n\t"
      "ld %[db], X+\n\t"
      "ld %[ch], X+\n\t"
      "ld %[cl], X+\n\t"
      "out %[portd], %[pd]\n\t"
      "out %[portb], %[pb]\n\t"
      "out %[ddrd], %[dd]\n\t"
      "out %[ddrb], %[db]\n\t"
      "out %[portc], %[ch]\n\t"
      "out %[portc], %[cl]\n\t"
      "rjmp .+0\n\t"
      "nop\n\t"
      "in %[d], %[pind]\n\t"
      "in %[b], %[pinb]\n\t"
      "out %[portc], %[ch]\n\t"
      "st Z+, %[d]\n\t"
      "st Z+, %[b]\n\t"
      "mov %[k], %[extra]\n\t"
      "2: dec %[k]\n\t"
      "brne 2b\n\t"
      "dec %[n]\n\t"
      "brne 1b\n\t"
      : [x] "+x"(x), [z] "+z"(z), [n] "+r"(n), [pd] "=&r"(pd), [pb] "=&r"(pb), [dd] "=&r"(dd), [db] "=&r"(db),
        [ch] "=&r"(ch), [cl] "=&r"(cl), [d] "=&r"(d), [b] "=&r"(b), [k] "=&r"(k)
      : [extra] "r"(extra), [portd] "I"(_SFR_IO_ADDR(PORTD)), [portb] "I"(_SFR_IO_ADDR(PORTB)),
        [ddrd] "I"(_SFR_IO_ADDR(DDRD)), [ddrb] "I"(_SFR_IO_ADDR(DDRB)), [portc] "I"(_SFR_IO_ADDR(PORTC)),
        [pind] "I"(_SFR_IO_ADDR(PIND)), [pinb] "I"(_SFR_IO_ADDR(PINB))
      : "memory");
  }
  dataRelease();
  PORTC = IDLE_C;
}

// Timer 1's overflows, counted by hand while interrupts are off; t1High when
// they are back on. Call at least every 4 ms.
static uint16_t heldHigh;

static void holdBegin() {
  cli();
  heldHigh = t1High;
  if (TIFR1 & _BV(TOV1)) { TIFR1 = _BV(TOV1); heldHigh++; }
}

static void holdEnd() {
  if (TIFR1 & _BV(TOV1)) { TIFR1 = _BV(TOV1); heldHigh++; }
  t1High = heldHigh;
  TIFR1 = _BV(ICF1);                                   // the capture interrupt has nothing to add
  sei();
}

static uint32_t heldNow() {
  const uint16_t low = TCNT1;
  uint16_t high = heldHigh;
  if ((TIFR1 & _BV(TOV1)) && low < 0x8000) high++;
  return ((uint32_t)high << 16) | low;
}

// The next falling edge of /INT, from Timer 1's input capture, with interrupts
// off: its time in `edge`, or false after `timeout` overflows (4.096 ms each).
static bool heldEdge(uint8_t timeout, uint32_t &edge) {
  uint8_t overflows = 0;
  for (;;) {
    const uint8_t f = TIFR1;
    if (f & _BV(TOV1)) {
      // A capture flagged beside an overflow came first if its count is high.
      if ((f & _BV(ICF1)) && ICR1 >= 0x8000) break;
      TIFR1 = _BV(TOV1);
      heldHigh++;
      if (++overflows > timeout) return false;
      continue;
    }
    if (f & _BV(ICF1)) break;
  }
  edge = ((uint32_t)heldHigh << 16) | ICR1;
  TIFR1 = _BV(ICF1);
  return true;
}

static void serveWhileIdle();

// Reads that differed, as SCRIPT logs them, from a run's samples. TRAFFIC's
// handler is served meanwhile, when armed.
static uint8_t compareRun(const uint8_t *pairs, uint8_t count, const uint8_t *raw, uint8_t *log, uint8_t differed) {
  for (uint8_t i = 0; i < count; i++) {
    if (!(i & 15)) serveWhileIdle();
    const uint8_t op = pairs[2 * i];
    if ((op & 0xC0) != 0x40) continue;
    const uint8_t got = sampled(raw + 2 * i);
    if (got == pairs[2 * i + 1]) continue;
    if (differed < SCRIPT_LOG) {
      log[3 * differed] = i;
      log[3 * differed + 1] = pairs[2 * i + 1];
      log[3 * differed + 2] = got;
    }
    if (differed < 255) differed++;
  }
  return differed;
}

// TRAFFIC's scanline handler (Phase 13's load): IRQLINE moved on by `step`,
// then STAT1 read to acknowledge, on one pair.
static uint8_t irqLine, irqStep, irqWrap, irqPair;
static uint32_t irqServiced;
static uint16_t irqLatencyMax;
static Entry handler[3];

// The handler as the Nano waits for the host, too: a round trip is
// milliseconds, dozens of lines, and TRAFFIC's handler has to keep up with an
// interrupt every eight. IDLE disarms it.
static void serviceScanline();

static void serveWhileIdle() {
  if (irqStep && !(PINB & _BV(PB0))) serviceScanline();
}

static void serviceScanline() {
  const uint16_t latency = (uint16_t)(TCNT1 - ICR1);
  irqLine = (uint8_t)(irqLine + irqStep);                // wrap 0 is 256
  if (irqWrap && irqLine >= irqWrap) irqLine = (uint8_t)(irqLine - irqWrap);
  const uint8_t command = (uint8_t)((irqPair ? 3 : 1) << 2);
  prepare(handler[0], command, irqLine);
  prepare(handler[1], command, 0x8B);                  // IRQLINE
  prepare(handler[2], (uint8_t)(0x80 | command), 0);   // STAT1, the pair's STATSEL
  // Interrupts off for the three accesses alone, 8 us: they keep PACED's
  // spacing, and serial loses no byte meanwhile.
  const uint8_t sreg = SREG;
  cli();
  pacedRun(handler, 3, 0, scratch + 2 * PACED_MAX);
  SREG = sreg;
  irqServiced++;
  if (latency > irqLatencyMax) irqLatencyMax = latency;
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
  CMD_SCRIPT = 0x0B,
  CMD_READ_RUN = 0x0C,
  CMD_PACED = 0x0D,
  CMD_INT_RUN = 0x0E,
  CMD_TRAFFIC = 0x0F,
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
    serveWhileIdle();
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
      irqStep = 0;
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
    case CMD_READ_RUN: {
      // port, count (1 to 120), extra: count reads of one port back to back
      // (readRun). Answers Timer 1's ticks for the run (u32), then the bytes.
      if (length != 3) { fail(sequence, ERR_LENGTH); return; }
      const uint8_t count = payload[1];
      if (count == 0 || count > BLOCK_MAX / 2) { fail(sequence, ERR_LENGTH); return; }
      uint8_t *raw = scratch;
      const uint32_t spent = readRun(payload[0], count, payload[2], raw + 4);
      raw[0] = (uint8_t)spent;
      raw[1] = (uint8_t)(spent >> 8);
      raw[2] = (uint8_t)(spent >> 16);
      raw[3] = (uint8_t)(spent >> 24);
      reply(type | ANSWER, sequence, raw, (uint8_t)(4 + count));
      return;
    }
    case CMD_SCRIPT: {
      // A run of accesses on any ports, with reads compared here so a batch
      // costs one round trip (Phase 11). Two bytes an access: op, value. op
      // b7:6 is 0 write `value`, 1 read and compare with `value`, 2 read and
      // ignore it, 3 a control; b3:2 the port, MODE1:MODE as PORTC has them.
      // A control's b3:2 is 0 to pulse /RESET low `value` us, 1 to wait `value`
      // us. Answers the number of reads that differed, saturating at 255, Timer
      // 1's ticks from the first access to the end of the last (u32), then up
      // to SCRIPT_LOG of the reads as (access index, expected, got).
      if (length == 0 || (length & 1)) { fail(sequence, ERR_LENGTH); return; }
      uint8_t out[5 + 3 * SCRIPT_LOG];
      const uint32_t began = ticksNow();
      const uint8_t differed = runScript(payload, length, out + 5);
      const uint32_t spent = ticksNow() - began;
      out[0] = differed;
      out[1] = (uint8_t)spent;
      out[2] = (uint8_t)(spent >> 8);
      out[3] = (uint8_t)(spent >> 16);
      out[4] = (uint8_t)(spent >> 24);
      const uint8_t logged = differed < SCRIPT_LOG ? differed : SCRIPT_LOG;
      reply(type | ANSWER, sequence, out, 5 + 3 * logged);
      return;
    }
    case CMD_PACED: {
      // extra, then (op, value) x 1 to PACED_MAX: pacedRun, reads compared as
      // SCRIPT compares them. extra b7 set answers the reads' bytes instead of
      // the log. Answers differed, Timer 1's ticks for the run (u32), then the
      // log or the bytes.
      if (length < 3 || !(length & 1) || (length - 1) / 2 > PACED_MAX) { fail(sequence, ERR_LENGTH); return; }
      const uint8_t extra = payload[0] & 0x7F;
      const bool bytes = payload[0] & 0x80;
      const uint8_t *pairs = payload + 1;
      const uint8_t count = (uint8_t)((length - 1) / 2);
      for (uint8_t i = 0; i < count; i++) {
        if ((pairs[2 * i] & 0xC0) == 0xC0) { fail(sequence, ERR_LENGTH); return; }
        prepare(entries[i], pairs[2 * i], pairs[2 * i + 1]);
      }
      holdBegin();
      const uint32_t began = heldNow();
      pacedRun(entries, count, extra, scratch);
      const uint32_t spent = heldNow() - began;
      holdEnd();
      uint8_t *out = answer;
      uint8_t size = 5;
      if (bytes) {
        out[0] = 0;
        for (uint8_t i = 0; i < count; i++) {
          if (pairs[2 * i] & 0xC0) out[size++] = sampled(scratch + 2 * i);
        }
      } else {
        out[0] = compareRun(pairs, count, scratch, out + 5, 0);
        size = (uint8_t)(5 + 3 * (out[0] < SCRIPT_LOG ? out[0] : SCRIPT_LOG));
      }
      out[1] = (uint8_t)spent;
      out[2] = (uint8_t)(spent >> 8);
      out[3] = (uint8_t)(spent >> 16);
      out[4] = (uint8_t)(spent >> 24);
      reply(type | ANSWER, sequence, out, size);
      return;
    }
    case CMD_INT_RUN: {
      // edges(2), timeout(1) in 4.096 ms overflows, flags(1), delay(2) in
      // ticks, extra(1), sequences(1), then each sequence: count(1) and its
      // (op, value) pairs. For each falling edge of /INT, sequence (edge mod
      // sequences) is played at pacedRun's spacing once `delay` ticks have
      // passed since the edge. flags b0: answer each edge's time (u32) and
      // the ticks from it to the run (u16); b1: answer each run's read bytes.
      // Answers edges taken (u16), 1 if a wait timed out, reads that differed
      // (u16), then the records.
      if (length < 9) { fail(sequence, ERR_LENGTH); return; }
      const uint16_t edges = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
      const uint8_t timeout = payload[2], flags = payload[3];
      const uint16_t delay = (uint16_t)payload[4] | ((uint16_t)payload[5] << 8);
      const uint8_t extra = payload[6], sequences = payload[7];
      if (!sequences || sequences > 4) { fail(sequence, ERR_LENGTH); return; }
      const uint8_t *seqPairs[4];
      uint8_t seqCount[4], seqFirst[4];
      uint8_t at = 8, total = 0;
      for (uint8_t q = 0; q < sequences; q++) {
        if (at >= length) { fail(sequence, ERR_LENGTH); return; }
        const uint8_t n = payload[at++];
        if (!n || total + n > PACED_MAX || at + 2 * n > length) { fail(sequence, ERR_LENGTH); return; }
        seqPairs[q] = payload + at;
        seqCount[q] = n;
        seqFirst[q] = total;
        for (uint8_t i = 0; i < n; i++) {
          if ((payload[at + 2 * i] & 0xC0) == 0xC0) { fail(sequence, ERR_LENGTH); return; }
          prepare(entries[total + i], payload[at + 2 * i], payload[at + 2 * i + 1]);
        }
        total = (uint8_t)(total + n);
        at = (uint8_t)(at + 2 * n);
      }
      if (at != length) { fail(sequence, ERR_LENGTH); return; }
      uint8_t *records = answer + 5;
      const uint8_t room = 250;
      uint8_t size = 0;
      uint16_t done = 0, differed = 0;
      bool timedOut = false;
      uint8_t log[3 * SCRIPT_LOG];
      // /INT already low is an edge not yet served: the capture interrupt has
      // its time. Edges from now on are caught with interrupts off.
      const bool asserted = !(PINB & _BV(PB0));
      const uint32_t before = intTicks();
      holdBegin();
      TIFR1 = _BV(ICF1);
      for (uint8_t q = 0; done < edges; q = (uint8_t)(q + 1 == sequences ? 0 : q + 1)) {
        uint32_t edge;
        if (done == 0 && asserted) edge = before;
        else if (!heldEdge(timeout, edge)) { timedOut = true; break; }
        const uint16_t low = (uint16_t)edge;
        while ((uint16_t)(TCNT1 - low) < delay) {
        }
        const uint16_t offset = (uint16_t)(TCNT1 - low);
        pacedRun(entries + seqFirst[q], seqCount[q], extra, scratch);
        const uint8_t d = compareRun(seqPairs[q], seqCount[q], scratch, log, 0);
        differed = (uint16_t)(differed + d);
        done++;
        // Whole records only, so the host can read them in step.
        uint8_t reads = 0;
        if (flags & 2) {
          for (uint8_t i = 0; i < seqCount[q]; i++) if (seqPairs[q][2 * i] & 0xC0) reads++;
        }
        const bool fits = size + ((flags & 1) ? 6 : 0) + reads <= room;
        if (!fits) continue;
        if (flags & 1) {
          records[size++] = (uint8_t)edge;
          records[size++] = (uint8_t)(edge >> 8);
          records[size++] = (uint8_t)(edge >> 16);
          records[size++] = (uint8_t)(edge >> 24);
          records[size++] = (uint8_t)offset;
          records[size++] = (uint8_t)(offset >> 8);
        }
        if (flags & 2) {
          for (uint8_t i = 0; i < seqCount[q]; i++) {
            if (seqPairs[q][2 * i] & 0xC0) records[size++] = sampled(scratch + 2 * i);
          }
        }
      }
      holdEnd();
      answer[0] = (uint8_t)done;
      answer[1] = (uint8_t)(done >> 8);
      answer[2] = timedOut;
      answer[3] = (uint8_t)differed;
      answer[4] = (uint8_t)(differed >> 8);
      reply(type | ANSWER, sequence, answer, (uint8_t)(5 + size));
      return;
    }
    case CMD_TRAFFIC: {
      // A paced batch, as PACED, with a scanline handler served before and
      // after it whenever /INT is low: IRQLINE moved on by `step`, wrapping
      // below `wrap` (0 for 256), and STAT1 read, on pair `pair`. step(1), wrap(1),
      // pair(1), extra(1), then the pairs; step 0 leaves the handler as it is
      // and serves nothing. Answers as PACED, then handlers served (u32) and
      // the longest from an edge to its handler (u16 ticks), both since the
      // handler was last set up.
      if (length < 6 || (length & 1) || (length - 4) / 2 > PACED_MAX) { fail(sequence, ERR_LENGTH); return; }
      if (payload[0] && (payload[0] != irqStep || payload[1] != irqWrap || payload[2] != irqPair)) {
        irqStep = payload[0];
        irqWrap = payload[1];
        irqPair = payload[2];
        irqLine = 0;
        irqServiced = 0;
        irqLatencyMax = 0;
      }
      const uint8_t extra = payload[3];
      const uint8_t *pairs = payload + 4;
      const uint8_t count = (uint8_t)((length - 4) / 2);
      for (uint8_t i = 0; i < count; i++) {
        if ((pairs[2 * i] & 0xC0) == 0xC0) { fail(sequence, ERR_LENGTH); return; }
        prepare(entries[i], pairs[2 * i], pairs[2 * i + 1]);
        serveWhileIdle();                                // the batch's setting up is not deaf to /INT
      }
      holdBegin();
      if (payload[0] && !(PINB & _BV(PB0))) serviceScanline();
      const uint32_t began = heldNow();
      pacedRun(entries, count, extra, scratch);
      const uint32_t spent = heldNow() - began;
      if (payload[0] && !(PINB & _BV(PB0))) serviceScanline();
      holdEnd();
      uint8_t *out = answer;
      out[0] = compareRun(pairs, count, scratch, out + 5, 0);
      uint8_t size = (uint8_t)(5 + 3 * (out[0] < SCRIPT_LOG ? out[0] : SCRIPT_LOG));
      out[1] = (uint8_t)spent;
      out[2] = (uint8_t)(spent >> 8);
      out[3] = (uint8_t)(spent >> 16);
      out[4] = (uint8_t)(spent >> 24);
      out[size++] = (uint8_t)irqServiced;
      out[size++] = (uint8_t)(irqServiced >> 8);
      out[size++] = (uint8_t)(irqServiced >> 16);
      out[size++] = (uint8_t)(irqServiced >> 24);
      out[size++] = (uint8_t)irqLatencyMax;
      out[size++] = (uint8_t)(irqLatencyMax >> 8);
      reply(type | ANSWER, sequence, out, size);
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
  serveWhileIdle();
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
  for (uint8_t i = 0; i < length; i++) {
    crc = crcByte(crc, body[i]);
    if (!(i & 15)) serveWhileIdle();                   // a big packet's CRC is a millisecond
  }
  const uint16_t want = (uint16_t)body[length] | ((uint16_t)body[length + 1] << 8);
  if (crc != want) { fail(sequence, ERR_CRC); return; }

  dispatch(type, sequence, body, length);
}
