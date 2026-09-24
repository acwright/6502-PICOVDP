// The bus. See bus.h.

#include "bus.h"


#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/structs/m33.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"

#include "bus.pio.h"

// PLAN.md section 5's pin map, which the stock firmware reports of itself
// (docs/results/phase-09.md). CD7, the bus's bit 0, is GPIO 14.
#define BUS_PIO pio1
#define WRITE_SM 0
#define READ_SM 1
#define GPIO_CD7 14
#define GPIO_INT 22
#define GPIO_RESET 23
#define GPIO_CSR tmsRead_CSR_PIN
#define GPIO_CSW tmsWrite_CSW_PIN
#define GPIO_MODE 28
#define GPIO_MODE1 29

// A restage that finds the staging FIFO full waits this long for the read
// program to take a word: it only stops draining for the length of a read.
#define STAGING_WAIT_CYCLES 1000

static vdp_t *card;

#if PICOVDP_DEBUG
static bus_stats_t counts;
static uint64_t lag_sum;
static uint64_t isr_sum;
static uint32_t isr_runs;
static volatile bool reset_request;
// The line start the next restage is the first for: the shared timer's count
// (renderer.c), or LAG_NONE.
#define LAG_NONE 0xffffffffu
static volatile uint32_t lag_start = LAG_NONE;
#define COUNT(field) (counts.field++)
#else
#define COUNT(field) ((void)0)
#endif

static inline uint32_t cycles(void) {
    return m33_hw->dwt_cyccnt;
}

// The word last staged. Only the bus's own interrupts stage, and they do not
// preempt each other: the staging FIFO has one writer, so the last word in it
// is the newest, and this is it.
static uint32_t staged;

#if PICOVDP_DEBUG
static uint32_t staged_at;  // DWT, as the last word was staged
static uint32_t entered_at; // DWT, as the bus interrupt last began
#define STAGED_NOW() (staged_at = cycles())
#else
#define STAGED_NOW() ((void)0)
#endif

static inline __attribute__((always_inline)) void stage(uint32_t word) {
    if (pio_sm_is_tx_fifo_full(BUS_PIO, READ_SM)) {
        COUNT(staging_waits);
        const uint32_t t0 = cycles();
        while (pio_sm_is_tx_fifo_full(BUS_PIO, READ_SM) && cycles() - t0 < STAGING_WAIT_CYCLES) {
        }
    }
    BUS_PIO->txf[READ_SM] = word;
    staged = word;
    STAGED_NOW();
}

// Stage what each port reads now, and drive /INT to match the card.
static void __scratch_x("bus_sync") bus_sync(void) {
    stage(vdp_staged(card));
    // §14: /INT is active low.
    if (vdp_int_asserted(card)) {
        sio_hw->gpio_clr = 1u << GPIO_INT;
    } else {
        sio_hw->gpio_set = 1u << GPIO_INT;
    }
#if PICOVDP_DEBUG
    const uint32_t start = lag_start;
    if (start != LAG_NONE) {
        lag_start = LAG_NONE;
        const uint32_t lag = sio_hw->mtime - start;
        counts.lag_count++;
        lag_sum += lag;
        if (lag > counts.lag_max) counts.lag_max = lag;
        uint32_t bin = lag / BUS_LAG_BIN;
        if (bin >= BUS_LAG_BINS) bin = BUS_LAG_BINS - 1;
        if (counts.lag_histogram[bin] != 0xffff) counts.lag_histogram[bin]++;
    }
#endif
}

// A whole restage asked for. The bus interrupt's pending bit is shared with
// the accesses that raise it, so the request is a flag of its own.
static volatile bool restage_requested;

void __scratch_x("bus_restage") bus_restage(void) {
    if (!card) return;  // no bus on this board
    restage_requested = true;
    irq_set_pending(PIO1_IRQ_0);
}

// What taking an access left to restage.
enum { RESTAGE_NONE, RESTAGE_DATA, RESTAGE_ALL };

// The oldest access the programs have handed over, if any, from the FIFOs'
// status as read. A write and a read both waiting came closer together than
// this interrupt can answer, and their order is lost: counted, and the write
// taken first, which is the order a program that sets an address and then
// reads through it depends on. The interrupt takes one access a run; the PIO
// holds it asserted while another waits, and it runs again.
//
// Most accesses move nothing a status port reads, nor /INT: a data read or
// write, which moves its pair's prefetch; a payload, which moves nothing; an
// address, which may move the prefetch. Those restage the two data bytes
// alone. A register write or a status read may move anything, and restages
// everything (Phase 13).
static inline __attribute__((always_inline)) unsigned take_one(uint32_t status) {
    const bool write = !(status & (1u << (PIO_FSTAT_RXEMPTY_LSB + WRITE_SM)));
    const bool read = !(status & (1u << (PIO_FSTAT_RXEMPTY_LSB + READ_SM)));
    if (write) {
        if (read) COUNT(coincident);
        // MODE1:MODE as /CSW fell, in bits 31:30; CD0-7 as it rose, in 7:0.
        const uint32_t word = BUS_PIO->rxf[WRITE_SM];
        const unsigned port = word >> 30;
        const uint8_t value = (uint8_t)word;
        vdp_write(card, port, value);
        COUNT(writes);
        // A command byte (the pair's flip-flop now clear) naming a register.
        return (port & 1) && !card->port[port >> 1].second && (value & 0x80) ? RESTAGE_ALL : RESTAGE_DATA;
    }
    if (!read) return RESTAGE_NONE;
    const uint32_t word = BUS_PIO->rxf[READ_SM];
    const uint8_t served = (uint8_t)word;
    const unsigned port = (word >> 8) & 3;
    COUNT(reads);
    if (!(port & 1)) {
        // §4's data read: what was served was the prefetch as staged.
        const uint8_t now = vdp_read(card, port);
        if (now != served) {
#if PICOVDP_DEBUG
            const unsigned k = counts.stale_data % BUS_STALE_KEPT;
            counts.stale[k].port = (uint8_t)port;
            counts.stale[k].served = served;
            counts.stale[k].held = now;
            counts.stale[k].staged = staged;
            counts.stale[k].since_stage = cycles() - staged_at;
            counts.stale[k].since_entry = cycles() - entered_at;
#endif
            COUNT(stale_data);
        }
        return RESTAGE_DATA;
    }
    if (vdp_read_served(card, port, served) != served) COUNT(stale_status);
    return RESTAGE_ALL;
}

static void __isr __scratch_x("bus_isr") bus_isr(void) {
#if PICOVDP_DEBUG
    const uint32_t t0 = cycles();
    entered_at = t0;
#endif
    const uint32_t status = BUS_PIO->fstat;
    unsigned restage = take_one(status);
    // A restage asked for by the latch, a publication or the thread restages
    // everything, whatever access came with it. Nothing that asks can run
    // while this does, so the flag is read and cleared in one.
    if (restage_requested) {
        restage_requested = false;
        restage = RESTAGE_ALL;
    }
    if (restage == RESTAGE_DATA && !(status & (1u << (PIO_FSTAT_TXFULL_LSB + READ_SM)))) {
        // The status bytes as last staged: nothing since has moved them, or
        // what did has asked for a whole restage, which follows this at once.
        // The staging FIFO had room as this began, and only this fills it.
        staged = (staged & 0xff00ff00u) | card->port[0].prefetch | (uint32_t)card->port[1].prefetch << 16;
        BUS_PIO->txf[READ_SM] = staged;
        STAGED_NOW();
    } else if (restage != RESTAGE_NONE) {
        bus_sync();
    }
#if PICOVDP_DEBUG
    const uint32_t spent = cycles() - t0;
    if (spent > counts.isr_max) counts.isr_max = spent;
    isr_sum += spent;
    isr_runs++;
#endif
}

// §15 on RST's falling edge. Accesses that came before it are taken first.
static void __isr __time_critical_func(reset_isr)(void) {
    gpio_acknowledge_irq(GPIO_RESET, GPIO_IRQ_EDGE_FALL);
    while (take_one(BUS_PIO->fstat) != RESTAGE_NONE) {
    }
    vdp_reset(card, false);
    COUNT(resets);
    bus_sync();
}

void __scratch_x("bus_line") bus_line(uint32_t line_start) {
    if (!card) return;
    const uint32_t mask = (1u << (PIO_FDEBUG_RXSTALL_LSB + WRITE_SM)) | (1u << (PIO_FDEBUG_RXSTALL_LSB + READ_SM)) |
                          (1u << (PIO_FDEBUG_TXOVER_LSB + READ_SM));
    const uint32_t seen = BUS_PIO->fdebug & mask;
    if (seen) {
        BUS_PIO->fdebug = seen;
#if PICOVDP_DEBUG
        if (seen & (1u << (PIO_FDEBUG_RXSTALL_LSB + WRITE_SM))) counts.write_overruns++;
        if (seen & (1u << (PIO_FDEBUG_RXSTALL_LSB + READ_SM))) counts.read_overruns++;
        if (seen & (1u << (PIO_FDEBUG_TXOVER_LSB + READ_SM))) counts.staging_waits++;
#endif
    }
#if PICOVDP_DEBUG
    if (reset_request) {
        const bus_stats_t zero = {0};
        counts = zero;
        lag_sum = 0;
        isr_sum = 0;
        isr_runs = 0;
        reset_request = false;
    }
    lag_start = line_start;
#else
    (void)line_start;
#endif
    bus_restage();
}

void bus_start(vdp_t *v) {
#ifndef PICO9918PRO
    // A Pico 2 has nothing on these pins, and GPIO 23 is its regulator's mode.
    (void)v;
    return;
#endif
    card = v;

    // /INT released before it is driven.
    gpio_init(GPIO_INT);
    gpio_put(GPIO_INT, 1);
    gpio_set_dir(GPIO_INT, GPIO_OUT);

    // The strobes, MODE, MODE1 and RST are inputs; the PIO reads them whatever
    // their function, once their pads are out of isolation.
    const unsigned inputs[] = {GPIO_CSR, GPIO_CSW, GPIO_MODE, GPIO_MODE1, GPIO_RESET};
    for (unsigned i = 0; i < sizeof inputs / sizeof inputs[0]; i++) gpio_init(inputs[i]);
    for (unsigned i = 0; i < 8; i++) pio_gpio_init(BUS_PIO, GPIO_CD7 + i);
    pio_sm_set_consecutive_pindirs(BUS_PIO, READ_SM, GPIO_CD7, 8, false);

    // tmsWrite as pico9918 configures it, with its unused TX FIFO joined to RX
    // for eight words of slack.
    // tmsRead first: its jump table has it at offset 0 (bus.pio).
    const unsigned read = pio_add_program(BUS_PIO, &tmsRead_program);
    const unsigned write = pio_add_program(BUS_PIO, &tmsWrite_program);
    pio_sm_config c = tmsWrite_program_get_default_config(write);
    sm_config_set_in_pins(&c, GPIO_CD7);
    sm_config_set_in_shift(&c, false, true, 32);
    sm_config_set_jmp_pin(&c, GPIO_CSW);
    sm_config_set_clkdiv(&c, 1.0f);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    pio_sm_init(BUS_PIO, WRITE_SM, write, &c);

    c = tmsRead_program_get_default_config(read);
    sm_config_set_jmp_pin(&c, GPIO_CSR);
    sm_config_set_in_pins(&c, GPIO_MODE);
    sm_config_set_out_pins(&c, GPIO_CD7, 8);
    sm_config_set_in_shift(&c, false, false, 32);
    sm_config_set_out_shift(&c, true, false, 32);
    sm_config_set_clkdiv(&c, 1.0f);
    pio_sm_init(BUS_PIO, READ_SM, read, &c);

    // The first word, before the program can answer anything.
    BUS_PIO->txf[READ_SM] = vdp_staged(card);
    BUS_PIO->fdebug = 0xffffffffu;

    // On core 1, above everything else there (bus.h). RST with it: neither
    // preempts the other.
    irq_set_exclusive_handler(PIO1_IRQ_0, bus_isr);
    irq_set_priority(PIO1_IRQ_0, PICO_HIGHEST_IRQ_PRIORITY);
    pio_set_irq0_source_mask_enabled(BUS_PIO,
                                     (1u << (pis_sm0_rx_fifo_not_empty + WRITE_SM)) |
                                         (1u << (pis_sm0_rx_fifo_not_empty + READ_SM)),
                                     true);
    irq_set_enabled(PIO1_IRQ_0, true);

    irq_set_exclusive_handler(IO_IRQ_BANK0, reset_isr);
    irq_set_priority(IO_IRQ_BANK0, PICO_HIGHEST_IRQ_PRIORITY);
    gpio_set_irq_enabled(GPIO_RESET, GPIO_IRQ_EDGE_FALL, true);
    irq_set_enabled(IO_IRQ_BANK0, true);

    pio_set_sm_mask_enabled(BUS_PIO, (1u << WRITE_SM) | (1u << READ_SM), true);
    bus_restage();
}

#if PICOVDP_DEBUG

void bus_stats(bus_stats_t *out, bool reset) {
    *out = counts;  // fields may move under it; each is whole
    out->int_level = !gpio_get_out_level(GPIO_INT);
    out->lag_mean = out->lag_count ? (uint32_t)(lag_sum / out->lag_count) : 0;
    out->isr_mean = isr_runs ? (uint32_t)(isr_sum / isr_runs) : 0;
    if (reset) reset_request = true;
}

#endif
