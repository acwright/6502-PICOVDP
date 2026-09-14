// Crashes and hangs (PLAN.md section 3, "The debug link").
//
// A HardFault on either core, or a panic, writes a record to RAM that a reset
// does not clear, and reboots through the watchdog. The watchdog, 250 ms, is
// fed from core 0 only while core 1's renderer and core 0's thread both make
// progress; when it bites, the next boot writes the record itself. After
// either, the firmware comes up in safe mode: USB up, the renderer off, every
// row one colour.

#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
    FAULT_NONE = 0,
    FAULT_HARDFAULT = 1,
    FAULT_PANIC = 2,
    FAULT_WATCHDOG = 3,
};

// The kinds of deliberate fault FAULT raises (debug builds).
enum {
    FAULT_RAISE_CORE0_HARDFAULT = 0,
    FAULT_RAISE_CORE1_HARDFAULT = 1,
    FAULT_RAISE_CORE1_HANG = 2,
    FAULT_RAISE_PANIC = 3,
};

#define FAULT_STACK_WORDS 32

typedef struct fault_record {
    uint32_t magic;
    uint32_t crc;                     // CRC-32 of the rest
    uint32_t kind;
    uint32_t core;
    uint32_t r0, r1, r2, r3, r12, lr, pc, xpsr;  // the exception frame
    uint32_t exc_return, sp;
    uint32_t cfsr, hfsr, mmfar, bfar, sfsr, sfar;
    uint32_t stack[FAULT_STACK_WORDS];
    uint64_t uptime_us;
    uint32_t heartbeat_core0, heartbeat_core1;
    char message[64];                 // a panic's, or what stopped the watchdog being fed
    char build[32];
} fault_record_t;

typedef enum {
    RESET_POWER_ON,                   // or anything else not below
    RESET_FAULT,                      // our own reboot after a fault record
    RESET_WATCHDOG,                   // the watchdog bit
} reset_reason_t;

// First thing at boot: why this boot happened, and whether it is safe mode.
reset_reason_t fault_boot(void);
reset_reason_t fault_boot_reason(void);
bool fault_safe_mode(void);

// The last record, if one is intact.
bool fault_last(fault_record_t *out);

// Core 0: start the watchdog and the timer that feeds it.
void fault_watchdog_start(uint32_t (*core1_heartbeat)(void));

// Core 0's thread counts each pass here; stalled for 2 s, the watchdog is not fed.
void fault_thread_alive(void);

// A deliberate fault, on the calling core.
void fault_raise(unsigned kind);

// Forget the record (REBOOT does).
void fault_clear(void);

uint32_t fault_crc32(uint32_t crc, const uint8_t *bytes, uint32_t count);
