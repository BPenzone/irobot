// Internal interfaces between the machine, math box and rasterizer.
//
// Copyright 2026 the irobot 32X port authors. GPL-3.0-or-later, see LICENSE.

#ifndef IROBOT_INTERNAL_H
#define IROBOT_INTERNAL_H

#include "irobot.h"

// Math box memory: 32K x 16-bit words in host byte order.
// Words 0x0000-0x0FFF are RAM shared with the 6809 at $2000-$3FFF,
// words 0x2000-0x7FFF are ROM from 136029-101 to -104.
extern uint16_t ir_mathbox_mem[0x8000];

// Screen center used by the projection, in native pixels.
#define IR_XMID (IR_SCREEN_W / 2)
#define IR_YMID (IR_SCREEN_H / 2 + 6)

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

#define IR_DLIST_WORDS  0x10000
#define IR_MAX_POINTS   63

typedef struct
{
    uint32_t used;
    int32_t words[IR_DLIST_WORDS];
} ir_dlist_t;

extern ir_dlist_t ir_dlist;

// Add one command; flushes to the current draw target first if full.
void ir_dlist_add(int type, int color, const int32_t *xy, int count);

// Draw everything in the display list into the current draw target and
// empty the list.
void ir_dlist_flush(void);

// Rasterizer entry points (raster.c).
void ir_raster_list(uint8_t *bitmap, const int32_t *words, uint32_t used, ir_stats_t *stats);

// Math box entry point (mathbox.c): runs the command in math RAM word 0.
void ir_mathbox_start(ir_stats_t *stats);

#endif
