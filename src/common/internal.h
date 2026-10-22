// Internal interfaces between the machine, math box and rasterizer.
//
// Copyright 2026 the irobot 32X port authors. GPL-3.0-or-later, see LICENSE.

#ifndef IROBOT_INTERNAL_H
#define IROBOT_INTERNAL_H

#include "irobot.h"

// Math box memory: 32K x 16-bit words in host byte order. Words
// 0x0000-0x0FFF are RAM shared with the 6809 at $2000-$3FFF (mirrored up to
// 0x1FFF), words 0x2000-0x7FFF are ROM from 136029-101 to -104. The ROM is a
// separate array so that on the 32X it can stay in cartridge ROM.
extern uint16_t ir_math_ram[0x1000];

// Math RAM in 64-word chunks, each with a bit per copy of it (the 32X keeps
// copies for the slave) that is set when the chunk changes.
#define IR_MATH_CHUNKS  64
extern uint8_t ir_math_dirty[IR_MATH_CHUNKS];
extern const uint16_t *ir_math_rom;

static inline uint16_t *ir_math_ptr(uint32_t addr)
{
    addr &= 0x7FFF;
    if (addr < 0x2000)
        return &ir_math_ram[addr & 0x0FFF];
    return (uint16_t *)&ir_math_rom[addr - 0x2000];
}

// One math box word as an lvalue.
#define M(addr) (*ir_math_ptr(addr))

// Signed 32-bit division. On the SH-2 the on-chip divider takes 39 cycles,
// several times faster than the library routine. Each SH-2 has its own,
// and no interrupt handler uses it.
#if defined(__SH2__)
static inline int32_t ir_div(int32_t a, int32_t b)
{
    volatile int32_t *divu = (volatile int32_t *)0xFFFFFF00;   // DVSR, DVDNT
    divu[0] = b;
    divu[1] = a;
    return divu[1];
}
#else
#define ir_div(a, b) ((a) / (b))
#endif

// Screen center used by the projection, in native pixels.
#define IR_XMID (IR_SCREEN_W / 2)
#define IR_YMID 128     // the video processor maps math box y 0 to line 128

// ---------------------------------------------------------------------------
// Display list
//
// The math box appends drawing commands here, and the rasterizer consumes
// them in order. On the 32X the master SH-2 fills the list and the slave
// SH-2 draws it. Each command is one header word followed by its points:
//
//   header = type << 12 | count << 6 | color   (count <= 63)
//   point  = x, y as int32
// ---------------------------------------------------------------------------

enum
{
    IR_CMD_ERASE   = 0,
    IR_CMD_POLYGON = 1,
    IR_CMD_VECTOR  = 2,   // closed polyline when count > 2
    IR_CMD_DOT     = 3,
};

// Value the rasterizer adds to every pixel it writes. The 32X ignores byte
// writes of zero to its frame buffer, so its build shifts the palette up one.
#ifndef IR_PIXEL_BASE
#define IR_PIXEL_BASE   0
#endif

// List entries are 16 bits: a command word (type << 12 | count << 6 |
// color), then `count` x, y pairs. Coordinates are clamped to 16 bits,
// far beyond anything the game draws.
typedef int16_t ir_dword_t;

#ifndef IR_DLIST_WORDS
#define IR_DLIST_WORDS  0x10000
#endif
#define IR_MAX_POINTS   63

// Add one command; flushes to the current draw target first if full.
void ir_dlist_add(int type, int color, const int32_t *xy, int count);

// Draw everything in the display list into the current draw target and
// empty the list.
void ir_dlist_flush(void);

// Rasterizer entry points (raster.c). `pitch` is the bitmap's line length
// in bytes; the picture is IR_SCREEN_W x IR_SCREEN_H.
void ir_raster_list(uint8_t *bitmap, int pitch, const ir_dword_t *words, uint32_t used, ir_stats_t *stats);

// ---------------------------------------------------------------------------
// Video backend
//
// The machine calls these; video_soft.c implements them for the PC harness
// with two bitmaps, and the 32X build hands the work to the slave SH-2.
// ---------------------------------------------------------------------------

// Start-up: forget any lists and clear both bitmaps.
void ir_video_reset(void);

// Draw a display list into bitmap `target` (0 or 1), or queue it to be drawn
// in order. Returns the buffer the machine should fill next, which may be
// the same one.
ir_dword_t *ir_video_submit(int target, ir_dword_t *words, uint32_t used, ir_stats_t *stats);

// Bitmap `shown` (0 or 1) is now the one on screen.
void ir_video_show(int shown);

// Color RAM entry `index` (0-63) changed to the given 8-bit RGB.
void ir_video_set_color(int index, uint8_t r, uint8_t g, uint8_t b);

// Math box entry point (mathbox.c): runs the command in math RAM word 0.
void ir_mathbox_start(ir_stats_t *stats);

// The commands that only draw: they read math RAM and add to the display
// list. ir_mathbox_draw runs one from `math_ram`, sending its output to `out`.
bool ir_mathbox_draws(uint16_t command);
void ir_mathbox_draw(const uint16_t *math_ram, void (*out)(int, int, const int32_t *, int), ir_stats_t *stats);

// The video backend may take a drawing command (math RAM word 0) to run
// elsewhere, in order with the display lists. Returns false to have the
// machine run it.
bool ir_video_mathbox(ir_stats_t *stats);

#endif
