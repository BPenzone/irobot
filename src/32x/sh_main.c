// SH-2 entry points for the 32X build.
//
// Copyright 2026 the irobot 32X port authors. GPL-3.0-or-later, see LICENSE.

#include <string.h>

#include "../common/irobot.h"
#include "shared.h"

// The prepared arcade ROM image, linked into cartridge ROM (image.s).
extern const ir_image_t ir_rom_image;

uint32_t master_vblank_count(void);

// Bring-up statistics, readable from a debugger.
volatile uint32_t stat_frames, stat_vblanks;

static void init_frame_buffers(void)
{
    while (!(MARS_SYS_INTMSK & MARS_SH2_ACCESS_VDP))
        ;
    MARS_VDP_DISPMODE = MARS_224_LINES | MARS_VDP_MODE_256;

    // Show 224 of the 232 arcade lines by dropping every 29th one.
    for (int pass = 0; pass < 2; pass++)
    {
        volatile uint16_t *fb = &MARS_FRAMEBUFFER;
        for (int y = 0; y < 224; y++)
        {
            int src = y * IR_SCREEN_H / 224;
            fb[y] = (uint16_t)((FB_FIRST_LINE + src * FB_LINE_BYTES) / 2);
        }
        for (int n = 0x100; n < 0x10000; n++)
            fb[n] = 0;

        int fs = MARS_VDP_FBCTL & MARS_VDP_FS;
        MARS_VDP_FBCTL = (uint16_t)(fs ^ 1);
        while ((MARS_VDP_FBCTL & MARS_VDP_FS) == fs)
            ;
    }
}

static void read_inputs(ir_inputs_t *in)
{
    uint16_t pad = COMM_PAD;
    memset(in, 0, sizeof *in);
    in->joy_x = in->joy_y = 0x80;
    if (pad & SEGA_CTRL_LEFT)  in->joy_x = 0x00;
    if (pad & SEGA_CTRL_RIGHT) in->joy_x = 0xFF;
    if (pad & SEGA_CTRL_UP)    in->joy_y = 0x00;
    if (pad & SEGA_CTRL_DOWN)  in->joy_y = 0xFF;
    in->fire = (pad & (SEGA_CTRL_B | SEGA_CTRL_C)) != 0;
    in->start1 = (pad & SEGA_CTRL_START) != 0;
    in->coin_left = (pad & SEGA_CTRL_A) != 0;
    in->start2 = (pad & SEGA_CTRL_MODE) != 0;
}

int m_main(void)
{
    init_frame_buffers();
    MARS_SYS_INTMSK |= 0x08;    // VBlank interrupt for palette updates

    ir_init(&ir_rom_image);

    uint32_t last = master_vblank_count();
    for (;;)
    {
        ir_inputs_t in;
        read_inputs(&in);
        ir_run_frame(&in);
        stat_frames++;

        // At most one arcade frame per 32X frame.
        while (master_vblank_count() == last)
            ;
        last = master_vblank_count();
        stat_vblanks = last;
    }
    return 0;
}

int s_main(void)
{
    slave_main_loop();
    return 0;
}
