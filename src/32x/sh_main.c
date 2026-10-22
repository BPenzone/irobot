// SH-2 entry points for the 32X build.
//
// Copyright 2026 the irobot 32X port authors. GPL-3.0-or-later, see LICENSE.

#include <string.h>

#include "../../third_party/mc6809/mc6809.h"
#include "../common/irobot.h"
#include "shared.h"

// The prepared arcade ROM image, linked into cartridge ROM (image.s).
extern const ir_image_t ir_rom_image;
extern mc6809__t ir_cpu;

uint32_t master_vblank_count(void);

// Bring-up statistics, readable from a debugger.
volatile uint32_t stat_frames, stat_vblanks;

static void init_frame_buffers(void)
{
    while (!(MARS_SYS_INTMSK & MARS_SH2_ACCESS_VDP))
        ;
    MARS_VDP_DISPMODE = MARS_224_LINES | MARS_VDP_MODE_256;

    // Show 224 of the 232 arcade lines. The dropped lines are the bottom
    // line of eight text rows spread down the screen: that line is blank in
    // almost every character, so the text stays intact.
    static const uint8_t dropped_rows[8] = { 2, 5, 9, 13, 16, 20, 24, 27 };
    for (int pass = 0; pass < 2; pass++)
    {
        volatile uint16_t *fb = &MARS_FRAMEBUFFER;
        int src = 0, next_drop = 0;
        for (int y = 0; y < 224; y++, src++)
        {
            if (next_drop < 8 && src == dropped_rows[next_drop] * 8 + 7)
            {
                src++;
                next_drop++;
            }
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

#ifdef IR_BENCH
// Benchmark build (make 32x BENCH=1): a fixed play session, timed in arcade
// frames rather than read from the pad, so that every build plays exactly
// the same game however fast it runs. It starts a game, walks the robot
// around the first levels and fires now and then.
static const struct { uint16_t frame, length, pad; } bench[] = {
    { 300, 8, SEGA_CTRL_START }, { 420, 8, SEGA_CTRL_A }, { 700, 30, SEGA_CTRL_RIGHT },
    { 900, 5, SEGA_CTRL_B }, { 1200, 200, SEGA_CTRL_UP }, { 1450, 20, SEGA_CTRL_LEFT },
    { 1500, 200, SEGA_CTRL_UP }, { 1750, 5, SEGA_CTRL_B }, { 1800, 300, SEGA_CTRL_UP },
    { 2150, 20, SEGA_CTRL_RIGHT }, { 2200, 300, SEGA_CTRL_UP }, { 2550, 5, SEGA_CTRL_B },
    { 2600, 300, SEGA_CTRL_UP },
};

static uint16_t bench_pad(void)
{
    uint16_t pad = 0;
    for (unsigned i = 0; i < sizeof bench / sizeof bench[0]; i++)
        if (stat_frames >= bench[i].frame && stat_frames < bench[i].frame + bench[i].length)
            pad |= bench[i].pad;
    return pad;
}
#undef COMM_PAD
#define COMM_PAD bench_pad()
#endif

static void read_inputs(ir_inputs_t *in)
{
    uint16_t pad = COMM_PAD;
    memset(in, 0, sizeof *in);
    in->joy_x = in->joy_y = 0x80;
    if (pad & SEGA_CTRL_LEFT)  in->joy_x = 0xFF;   // the arcade stick reads high to the left
    if (pad & SEGA_CTRL_RIGHT) in->joy_x = 0x00;
    if (pad & SEGA_CTRL_UP)    in->joy_y = 0x00;
    if (pad & SEGA_CTRL_DOWN)  in->joy_y = 0xFF;
    // The arcade's start buttons also change the view during play.
    in->fire = (pad & SEGA_CTRL_B) != 0;
    in->start1 = (pad & SEGA_CTRL_A) != 0;      // 1 player, view down
    in->start2 = (pad & SEGA_CTRL_C) != 0;      // 2 players, view up
    in->coin_left = (pad & SEGA_CTRL_START) != 0;
}

int m_main(void)
{
    // Recompiled code finds the CPU state through r14 (rc_runtime.h).
    __asm__ volatile ("mov %0,r14" : : "r"(&ir_cpu));
    init_frame_buffers();
    MARS_SYS_INTMSK |= 0x08;    // VBlank interrupt for palette updates

    ir_use_recompiled = true;
    ir_init(&ir_rom_image);

    // One arcade frame per 32X frame on average. A frame that runs long is
    // made up by starting the next one at once rather than waiting for a
    // VBlank; more than six frames behind, the debt is dropped.
    uint32_t due = master_vblank_count();
    for (;;)
    {
        ir_inputs_t in;
        read_inputs(&in);
        ir_run_frame(&in);
        sound_queue_frame();
        stat_frames++;

        due++;
        uint32_t now;
        while ((int32_t)(due - (now = master_vblank_count())) > 0)
            ;
        if ((int32_t)(now - due) > 6)
            due = now;
        stat_vblanks = now;
    }
    return 0;
}

int s_main(void)
{
    sound_init_slave();
    slave_main_loop();
    return 0;
}
