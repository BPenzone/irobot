// State shared between the two SH-2s and the 68000 on the 32X.
//
// Copyright 2026 the irobot 32X port authors. GPL-3.0-or-later, see LICENSE.

#ifndef IROBOT_32X_SHARED_H
#define IROBOT_32X_SHARED_H

#include <stdint.h>

#include "../../third_party/marsdev/mars.h"

// Each SH-2 has its own write-through cache, so anything one CPU reads
// after the other wrote it goes through the cache-through mirror of SDRAM.
// The result is volatile so polling loops reread memory.
#define UNCACHED(p) ((volatile __typeof__(*(p)) *)((uintptr_t)(p) | 0x20000000))

// Communication registers, as the SH-2s see them. The 68000 sees the same
// registers at $A15120.
#define COMM_PAD        MARS_SYS_COMM8      // 68000 -> SH-2: pad 1, SEGA_CTRL_* bits
#define COMM_TICKS      MARS_SYS_COMM12     // 68000 -> SH-2: VBlank count (32 bits)

// Where the arcade picture sits in each 32X frame buffer: 256-color mode,
// 320-byte lines after the 256-word line table, picture centered.
#define FB_LINE_BYTES   320
#define FB_FIRST_LINE   0x200               // byte offset of line 0
#define FB_X_OFFSET     ((320 - 256) / 2)

// Slave command queue (filled by the master, see video_32x.c).
enum
{
    SLAVE_DRAW = 1,     // draw display list `arg`
    SLAVE_FLIP,         // overlay the alphanumerics, then show the back buffer
};

void slave_main_loop(void);

#endif
