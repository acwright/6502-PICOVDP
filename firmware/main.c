// Phase 0 hello: proves the RP2350 build and, in debug builds, USB CDC logging
// and picotool's reset-into-BOOTSEL (PLAN.md section 3, "The debug link").
// Replaced by the real firmware in Phase 8.

#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "hardware/clocks.h"

int main(void) {
    stdio_init_all();

    char id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    pico_get_unique_board_id_string(id, sizeof id);

    for (unsigned n = 0;; n++) {
        printf("picovdp %s %s board %s id %s sys %lu Hz uptime %llu ms tick %u\n",
               PICOVDP_BUILD_ID, PICOVDP_DEBUG ? "debug" : "release", PICO_BOARD, id,
               (unsigned long)clock_get_hz(clk_sys),
               (unsigned long long)(time_us_64() / 1000), n);
        sleep_ms(1000);
    }
}
