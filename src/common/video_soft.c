// Software video backend for the PC harness: two 8-bit bitmaps drawn by the
// rasterizer as soon as each display list is submitted.
//
// Copyright 2026 the irobot 32X port authors. GPL-3.0-or-later, see LICENSE.

#include <string.h>

#include "internal.h"

static uint8_t bitmap[2][IR_SCREEN_W * IR_SCREEN_H];
static uint32_t palette[64];
static int shown;

void ir_video_reset(void)
{
    memset(bitmap, 0, sizeof bitmap);
    memset(palette, 0, sizeof palette);
    shown = 0;
}

ir_dword_t *ir_video_submit(int target, ir_dword_t *words, uint32_t used, ir_stats_t *stats)
{
    ir_raster_list(bitmap[target], IR_SCREEN_W, words, used, stats);
    return words;
}

bool ir_video_mathbox(ir_stats_t *stats)
{
    (void)stats;
    return false;
}

void ir_video_show(int index)
{
    shown = index;
}

void ir_video_set_color(int index, uint8_t r, uint8_t g, uint8_t b)
{
    palette[index] = (uint32_t)(r << 16 | g << 8 | b);
}

// Set by the harness to show the reference video processor's bitmap instead.
const uint8_t *(*ir_display_override)(void);

const uint8_t *ir_display_bitmap(void)
{
    return ir_display_override ? ir_display_override() : bitmap[shown];
}
const uint32_t *ir_bitmap_palette(void) { return palette; }

void ir_compose_rgb(uint32_t *out)
{
    const uint8_t *bm = ir_display_bitmap();
    for (int n = 0; n < IR_SCREEN_W * IR_SCREEN_H; n++)
        out[n] = palette[bm[n] & 0x3F];

    const uint8_t *alpha_ram = ir_alpha_ram();
    const uint8_t *charset = ir_alpha_charset();
    const uint32_t *pal = ir_alpha_palette();
    for (int row = 0; row < IR_SCREEN_H / 8; row++)
        for (int col = 0; col < IR_SCREEN_W / 8; col++)
        {
            uint8_t v = alpha_ram[row * 32 + col];
            int ch = v & 0x3F;
            if (!ch)
                continue;
            uint32_t color = pal[v >> 6];
            for (int y = 0; y < 8; y++)
            {
                uint8_t bits = charset[ch * 8 + y];
                uint32_t *dst = &out[(row * 8 + y) * IR_SCREEN_W + col * 8];
                for (int x = 0; x < 8; x++)
                    if (bits & (0x80 >> x))
                        dst[x] = color;
            }
        }
}
