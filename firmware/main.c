// 6502-PICOVDP firmware: SPEC.md on an RP2350 (PLAN.md section 3).
//
// Phase 8: everything but the bus. Core 0 runs the raster and, in debug
// builds, the debug link; core 1 latches and renders (renderer.h). A Pico 2
// drives the VGA pins with nothing on them; the PRO (Phase 10) drives its DAC.
// Phase 11: the bus, on core 1 beside the latch (bus.h), on the PRO only.

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"

#include "fault.h"
#include "renderer.h"

#if PICOVDP_DEBUG
#include "link.h"
#endif

// Safe mode's picture: every row dark red, which a capture can see.
#define SAFE_MODE_BGR 0x008

int main(void) {
    // Phase 1's clock: 352 MHz, pico9918's VGA preset 2. The voltage first.
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    sleep_ms(10);
    set_sys_clock_pll(1056000000, 3, 1);

    fault_boot();

#if PICOVDP_DEBUG
    // USB straight after the clocks, so an image that dies later can still be
    // reflashed without the BOOT button (risk 4).
    stdio_init_all();
    if (fault_safe_mode()) {
        renderer_start_safe(SAFE_MODE_BGR);
        fault_watchdog_start(NULL);
        link_run(true);
    }
#endif

    renderer_init();
    renderer_start();
    fault_watchdog_start(renderer_heartbeat);

#if PICOVDP_DEBUG
    link_run(false);
#else
    for (;;) {
        fault_thread_alive();
        sleep_ms(100);
    }
#endif
}
