// Crashes and hangs. See fault.h.

#include "fault.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "hardware/structs/m33.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/multicore.h"
#include "pico/platform.h"
#include "pico/stdlib.h"

#define RECORD_MAGIC 0x50564652u     // "PVFR"
#define SCRATCH_FAULT 0              // watchdog scratch 0: our reboot after a record
#define SCRATCH_FAULT_MAGIC 0x6502fa17u
#define WATCHDOG_MS 250
#define FEED_MS 50
#define THREAD_STALL_MS 2000

// Survives a watchdog reboot: not zeroed at boot.
static fault_record_t __uninitialized_ram(record);

// What the feeder last saw, for the record a watchdog bite leaves.
typedef struct breadcrumb {
    uint32_t magic;
    uint32_t heartbeat_core0, heartbeat_core1;
    uint64_t uptime_us;
    char reason[64];
} breadcrumb_t;
static breadcrumb_t __uninitialized_ram(breadcrumb);

static reset_reason_t reason;
static bool safe;
static uint32_t (*core1_heartbeat)(void);
static volatile uint32_t thread_passes;

uint32_t fault_crc32(uint32_t crc, const uint8_t *bytes, uint32_t count) {
    crc = ~crc;
    while (count--) {
        crc ^= *bytes++;
        for (unsigned k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

static uint32_t record_crc(const fault_record_t *r) {
    return fault_crc32(0, (const uint8_t *)&r->kind, sizeof *r - offsetof(fault_record_t, kind));
}

static void seal(fault_record_t *r) {
    snprintf(r->build, sizeof r->build, "%s", PICOVDP_BUILD_ID);
    r->magic = RECORD_MAGIC;
    r->crc = record_crc(r);
}

bool fault_last(fault_record_t *out) {
    if (record.magic != RECORD_MAGIC || record.crc != record_crc(&record)) return false;
    *out = record;
    return true;
}

void fault_clear(void) {
    record.magic = 0;
    breadcrumb.magic = 0;
}

reset_reason_t fault_boot(void) {
    if (watchdog_hw->scratch[SCRATCH_FAULT] == SCRATCH_FAULT_MAGIC) {
        reason = RESET_FAULT;
    } else if (watchdog_enable_caused_reboot()) {
        // The watchdog bit: nothing got to write a record, so write it now
        // from what the feeder last saw.
        reason = RESET_WATCHDOG;
        memset(&record, 0, sizeof record);
        record.kind = FAULT_WATCHDOG;
        record.core = 0xffffffffu;
        if (breadcrumb.magic == RECORD_MAGIC) {
            record.heartbeat_core0 = breadcrumb.heartbeat_core0;
            record.heartbeat_core1 = breadcrumb.heartbeat_core1;
            record.uptime_us = breadcrumb.uptime_us;
            memcpy(record.message, breadcrumb.reason, sizeof record.message);
        } else {
            snprintf(record.message, sizeof record.message, "watchdog: no breadcrumb");
        }
        seal(&record);
    } else {
        reason = RESET_POWER_ON;
    }
    watchdog_hw->scratch[SCRATCH_FAULT] = 0;
    breadcrumb.magic = 0;
    safe = reason != RESET_POWER_ON;
    return reason;
}

reset_reason_t fault_boot_reason(void) {
    return reason;
}

bool fault_safe_mode(void) {
    return safe;
}

// ---- the watchdog ----

static uint32_t last_core1, last_core0;
static uint64_t core0_seen_us;

static bool feed(repeating_timer_t *timer) {
    (void)timer;
    const uint32_t h1 = core1_heartbeat ? core1_heartbeat() : 0;
    const uint32_t h0 = thread_passes;
    const uint64_t now = time_us_64();
    if (h0 != last_core0) core0_seen_us = now;
    const bool core1_alive = !core1_heartbeat || h1 != last_core1;
    const bool core0_alive = now - core0_seen_us < THREAD_STALL_MS * 1000ull;

    breadcrumb.heartbeat_core0 = h0;
    breadcrumb.heartbeat_core1 = h1;
    breadcrumb.uptime_us = now;
    if (core1_alive && core0_alive) {
        watchdog_update();
    } else {
        snprintf(breadcrumb.reason, sizeof breadcrumb.reason, "watchdog: %s stalled",
                 core1_alive ? "core 0's thread" : "core 1's renderer");
    }
    breadcrumb.magic = RECORD_MAGIC;
    last_core1 = h1;
    last_core0 = h0;
    return true;
}

void fault_watchdog_start(uint32_t (*heartbeat)(void)) {
    static repeating_timer_t timer;
    core1_heartbeat = heartbeat;
    core0_seen_us = time_us_64();
    watchdog_enable(WATCHDOG_MS, true);
    add_repeating_timer_ms(FEED_MS, feed, NULL, &timer);
}

void fault_thread_alive(void) {
    thread_passes++;
}

// ---- faults ----

static void __attribute__((noreturn)) reboot_with_record(void) {
    seal(&record);
    watchdog_hw->scratch[SCRATCH_FAULT] = SCRATCH_FAULT_MAGIC;
    watchdog_reboot(0, 0, 0);
    for (;;) {
    }
}

// Called from isr_hardfault with the stacked frame and EXC_RETURN.
void __attribute__((used, noreturn)) fault_hardfault(uint32_t *frame, uint32_t exc_return) {
    (void)save_and_disable_interrupts();
    memset(&record, 0, sizeof record);
    record.kind = FAULT_HARDFAULT;
    record.core = get_core_num();
    record.exc_return = exc_return;
    record.sp = (uint32_t)frame;
    record.r0 = frame[0];
    record.r1 = frame[1];
    record.r2 = frame[2];
    record.r3 = frame[3];
    record.r12 = frame[4];
    record.lr = frame[5];
    record.pc = frame[6];
    record.xpsr = frame[7];
    record.cfsr = m33_hw->cfsr;
    record.hfsr = m33_hw->hfsr;
    record.mmfar = m33_hw->mmfar;
    record.bfar = m33_hw->bfar;
    record.sfsr = m33_hw->sfsr;
    record.sfar = m33_hw->sfar;
    for (unsigned i = 0; i < FAULT_STACK_WORDS; i++) record.stack[i] = frame[i];
    record.uptime_us = time_us_64();
    record.heartbeat_core0 = thread_passes;
    record.heartbeat_core1 = core1_heartbeat ? core1_heartbeat() : 0;
    reboot_with_record();
}

// The exception frame is on whichever stack EXC_RETURN bit 2 names.
void __attribute__((naked)) isr_hardfault(void) {
    pico_default_asm(
        "movs r0, #4\n"
        "mov r1, lr\n"
        "tst r0, r1\n"
        "beq 1f\n"
        "mrs r0, psp\n"
        "b 2f\n"
        "1: mrs r0, msp\n"
        "2: b fault_hardfault\n");
}

// PICO_PANIC_FUNCTION: a panic is a record too.
void __attribute__((noreturn)) picovdp_panic(const char *format, ...) {
    (void)save_and_disable_interrupts();
    memset(&record, 0, sizeof record);
    record.kind = FAULT_PANIC;
    record.core = get_core_num();
    record.lr = (uint32_t)__builtin_return_address(0);
    if (format) {
        va_list args;
        va_start(args, format);
        vsnprintf(record.message, sizeof record.message, format, args);
        va_end(args);
    }
    record.uptime_us = time_us_64();
    record.heartbeat_core0 = thread_passes;
    record.heartbeat_core1 = core1_heartbeat ? core1_heartbeat() : 0;
    reboot_with_record();
}

void fault_raise(unsigned kind) {
    switch (kind) {
    case FAULT_RAISE_CORE0_HARDFAULT:
    case FAULT_RAISE_CORE1_HARDFAULT: {
        // An undefined instruction: a UsageFault, escalated.
        __builtin_trap();
    }
    case FAULT_RAISE_CORE1_HANG:
        (void)save_and_disable_interrupts();
        for (;;) {
        }
    case FAULT_RAISE_PANIC:
        panic("FAULT raised a panic on core %u", get_core_num());
    }
}
