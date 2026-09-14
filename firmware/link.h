// The debug link: framed packets over USB CDC (docs/DEBUGLINK.md). Core 0's
// thread, at the bottom of core 0's priorities: the raster's interrupts and
// the sprite jobs preempt it, and USB's own interrupts are below both.

#pragma once

#include <stdbool.h>

// Serve the link forever. In safe mode only INFO, REBOOT and FAULT answer.
void link_run(bool safe_mode);
