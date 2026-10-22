// 68000 side of the 32X build: reads the pad for the SH-2s once per frame.
//
// Copyright 2026 the irobot 32X port authors. GPL-3.0-or-later, see LICENSE.

#include <stdint.h>

#define MARS_COMM8  ((volatile uint16_t *)0xA15128)
#define MARS_COMM12 ((volatile uint32_t *)0xA1512C)
#define VDP_CTRL    ((volatile uint16_t *)0xC00004)

extern uint16_t read_joypad(uint8_t player);

__attribute__((section(".data")))
void main(void)
{
    uint32_t ticks = 0;
    for (;;)
    {
        while (*VDP_CTRL & 8)       // wait for the end of VBlank
            ;
        while (!(*VDP_CTRL & 8))    // then for the next one
            ;
        *MARS_COMM8 = read_joypad(0);
        *MARS_COMM12 = ++ticks;
    }
}
