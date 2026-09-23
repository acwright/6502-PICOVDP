// The bus. See bus.h.

#include "bus.h"


#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/structs/m33.h"
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
static volatile bool reset_request;
#define COUNT(field) (counts.field++)
#else
#define COUNT(field) ((void)0)
#endif

static inline uint32_t cycles(void) {
    return m33_hw->dwt_cyccnt;
}

void __time_critical_func(bus_sync)(void) {
    if (!card) return;  // no bus on this board
    const uint32_t word = vdp_staged(card);
    if (pio_sm_is_tx_fifo_full(BUS_PIO, READ_SM)) {
        COUNT(staging_waits);
        const uint32_t t0 = cycles();
        while (pio_sm_is_tx_fifo_full(BUS_PIO, READ_SM) && cycles() - t0 < STAGING_WAIT_CYCLES) {
        }
    }
    BUS_PIO->txf[READ_SM] = word;
    // §14: /INT is active low.
    if (vdp_int_asserted(card)) {
        sio_hw->gpio_clr = 1u << GPIO_INT;
    } else {
        sio_hw->gpio_set = 1u << GPIO_INT;
    }
}

// Every access the programs have handed over, in the order they arrived as far
// as two FIFOs can tell it. A write and a read both waiting came closer together
// than this interrupt can answer, and their order is lost: counted, and taken
// write first, which is the order a program that sets an address and then reads
// through it depends on.
static void __time_critical_func(take_accesses)(void) {
    for (;;) {
        const bool write = !pio_sm_is_rx_fifo_empty(BUS_PIO, WRITE_SM);
        const bool read = !pio_sm_is_rx_fifo_empty(BUS_PIO, READ_SM);
        if (!write && !read) return;
        if (write && read) COUNT(coincident);
        if (write) {
            // MODE1:MODE as /CSW fell, in bits 31:30; CD0-7 as it rose, in 7:0.
            const uint32_t word = BUS_PIO->rxf[WRITE_SM];
            vdp_write(card, word >> 30, (uint8_t)word);
            COUNT(writes);
        } else {
            const uint32_t word = BUS_PIO->rxf[READ_SM];
            const uint8_t served = (uint8_t)word;
            const unsigned port = (word >> 8) & 3;
            if (vdp_read_served(card, port, served) != served) {
                if (port & 1) COUNT(stale_status);
                else COUNT(stale_data);
            }
            COUNT(reads);
        }
    }
}

static void __isr __time_critical_func(bus_isr)(void) {
#if PICOVDP_DEBUG
    const uint32_t t0 = cycles();
#endif
    take_accesses();
    bus_sync();
#if PICOVDP_DEBUG
    const uint32_t spent = cycles() - t0;
    if (spent > counts.isr_max) counts.isr_max = spent;
#endif
}

// §15 on RST's falling edge. Accesses that came before it are taken first.
static void __isr __time_critical_func(reset_isr)(void) {
    gpio_acknowledge_irq(GPIO_RESET, GPIO_IRQ_EDGE_FALL);
    take_accesses();
    vdp_reset(card, false);
    COUNT(resets);
    bus_sync();
}

void __time_critical_func(bus_line)(void) {
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
        reset_request = false;
    }
#endif
    bus_sync();
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

    // On core 1, at the latch's priority, so neither preempts the other.
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
    bus_sync();
}

#if PICOVDP_DEBUG

void bus_stats(bus_stats_t *out, bool reset) {
    *out = counts;  // fields may move under it; each is whole
    out->int_level = !gpio_get_out_level(GPIO_INT);
    if (reset) reset_request = true;
}

#endif
