// Software rasterizer for the display list: flat polygons, lines and dots
// into an 8-bit bitmap of IR_SCREEN_W x IR_SCREEN_H.
//
// Copyright 2026 the irobot 32X port authors. GPL-3.0-or-later, see LICENSE.
//
// Written to run on the 32X slave SH-2 later, so it uses only integer math
// and avoids 64-bit arithmetic in the inner loops.

#include <string.h>

#include "internal.h"

#define W IR_SCREEN_W
#define H IR_SCREEN_H

static int pitch;

// memset for spans: the 32X frame buffer takes a 32-bit write as quickly as
// a byte, and this stays in fast RAM with the rest of the rasterizer.
static inline void fill(uint8_t *p, uint8_t c, int n)
{
    while (n > 0 && ((uintptr_t)p & 3))
    {
        *p++ = c;
        n--;
    }
    uint32_t c4 = c * 0x01010101u;
    uint32_t *q = (uint32_t *)p;
    for (; n >= 16; n -= 16, q += 4)
    {
        q[0] = c4;
        q[1] = c4;
        q[2] = c4;
        q[3] = c4;
    }
    for (; n >= 4; n -= 4)
        *q++ = c4;
    p = (uint8_t *)q;
    while (n-- > 0)
        *p++ = c;
}

static inline void plot(uint8_t *bm, int32_t x, int32_t y, uint8_t c)
{
    if ((uint32_t)x < W && (uint32_t)y < H)
        bm[y * pitch + x] = c;
}

// Clip a line to the screen (Liang-Barsky). Returns false if it misses.
static bool clip_line(int32_t *x0, int32_t *y0, int32_t *x1, int32_t *y1)
{
    int64_t dx = (int64_t)*x1 - *x0, dy = (int64_t)*y1 - *y0;
    // t = num / den as fractions, kept in 16.16 fixed point
    int64_t t0 = 0, t1 = 1 << 16;
    const int64_t p[4] = { -dx, dx, -dy, dy };
    const int64_t q[4] = { *x0, (W - 1) - (int64_t)*x0, *y0, (H - 1) - (int64_t)*y0 };

    for (int i = 0; i < 4; i++)
    {
        if (p[i] == 0)
        {
            if (q[i] < 0)
                return false;
            continue;
        }
        int64_t t = (q[i] << 16) / p[i];
        if (p[i] < 0)
        {
            if (t > t1) return false;
            if (t > t0) t0 = t;
        }
        else
        {
            if (t < t0) return false;
            if (t < t1) t1 = t;
        }
    }
    int32_t ox = *x0, oy = *y0;
    *x0 = (int32_t)(ox + ((dx * t0) >> 16));
    *y0 = (int32_t)(oy + ((dy * t0) >> 16));
    *x1 = (int32_t)(ox + ((dx * t1) >> 16));
    *y1 = (int32_t)(oy + ((dy * t1) >> 16));
    return true;
}

static void draw_line(uint8_t *bm, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t c)
{
    if (!clip_line(&x0, &y0, &x1, &y1))
        return;

    int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int32_t dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int32_t sx = x0 < x1 ? 1 : -1;
    int32_t sy = y0 < y1 ? 1 : -1;
    int32_t err = dx - dy;

    for (;;)
    {
        plot(bm, x0, y0, c);
        if (x0 == x1 && y0 == y1)
            break;
        int32_t e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

// Polygons are filled the way the 1998 I, Robot emulator does it (after
// Michael Abrash's fast polygon fill): starting at the top vertex, walk one
// chain of edges as the left side and the other chain as the right side down
// to the bottom vertex, appending one x per scanline to each side's list in
// the order the edges are walked. Edges that run upward add nothing. For a
// polygon that isn't monotone in y this is not a plain outline fill, and the
// game relies on that: the lettering on the shield ("REDS") is drawn as such
// polygons.

static int16_t edge_left[H], edge_right[H];

// Append the x positions of one edge, one per scanline from y0 (skipped if
// skip_first) to y1 (exclusive), clipped to the screen. Returns the new
// list length.
static int scan_edge(int16_t *list, int n, int32_t x0, int32_t y0, int32_t x1, int32_t y1, bool skip_first)
{
    if (y0 >= H || y1 < 0)
        return n;
    int32_t lines_to_skip = 0;
    if (y0 < 0)
    {
        lines_to_skip = -y0;
        skip_first = false;     // the first line is off screen anyway
    }
    int32_t height = y1 - y0;
    int32_t lines = (y1 < H ? y1 : H - 1) - (y0 < 0 ? 0 : y0);
    if (lines <= 0)
        return n;

    // Bresenham-style stepping: x advances by whole + an error term.
    int32_t dx = x1 - x0, dir = 1, err = 0;
    if (dx < 0)
    {
        dir = -1;
        dx = -dx;
        err = 1 - height;
    }
    int32_t whole = ir_div(dx, height), frac = dx - whole * height;
    int32_t x = x0;

    if (lines_to_skip)
    {
        // As many steps of the loop below: err gains frac each line and
        // gives back height, moving x one more, whenever it goes above 0.
        uint32_t k = (uint32_t)lines_to_skip;
        uint32_t extra = (k * (uint32_t)frac + (uint32_t)(err + height - 1)) / (uint32_t)height;
        x += (int32_t)(k * (uint32_t)whole + extra) * dir;
        err += (int32_t)(k * (uint32_t)frac - extra * (uint32_t)height);
    }
    for (int32_t i = 0; i < lines; i++)
    {
        if (!(i == 0 && skip_first) && n < H)
            list[n++] = (int16_t)(x < -1 ? -1 : x > W ? W : x);
        x += whole * dir;
        err += frac;
        if (err > 0) { x += dir; err -= height; }
    }
    return n;
}

static void draw_polygon(uint8_t *bm, const int32_t *pts, int count, uint8_t c)
{
#define PX(i) pts[(i) * 2]
#define PY(i) pts[(i) * 2 + 1]
#define FWD(i) ((i) + 1 == count ? 0 : (i) + 1)
#define BACK(i) ((i) == 0 ? count - 1 : (i) - 1)
    int min_l = 0, max_i = 0;
    int32_t min_y = PY(0), max_y = PY(0);
    for (int i = 1; i < count; i++)
    {
        if (PY(i) < min_y)
            min_y = PY(min_l = i);
        else if (PY(i) > max_y)
            max_y = PY(max_i = i);
    }
    if (min_y == max_y)
        return;

    // The top may be a flat run of vertices; find both ends of it.
    int min_r = min_l;
    while (PY(min_r) == min_y)
        min_r = FWD(min_r);
    min_r = BACK(min_r);
    while (PY(min_l) == min_y)
        min_l = BACK(min_l);
    min_l = FWD(min_l);

    // Which way round the vertex list is the left side?
    int left_dir = -1;
    bool top_is_flat = PX(min_l) != PX(min_r);
    if (top_is_flat)
    {
        if (PX(min_l) > PX(min_r))
        {
            left_dir = 1;
            int t = min_l; min_l = min_r; min_r = t;
        }
    }
    else
    {
        int next = FWD(min_r), prev = BACK(min_l);
        int64_t dxn = PX(next) - PX(min_l), dyn = PY(next) - PY(min_l);
        int64_t dxp = PX(prev) - PX(min_l), dyp = PY(prev) - PY(min_l);
        if (dxn * dyp - dyn * dxp < 0)
        {
            left_dir = 1;
            int t = min_l; min_l = min_r; min_r = t;
        }
    }

    // A pointed top vertex belongs to the line below it.
    int32_t y_start = min_y + 1 - top_is_flat;
    if (y_start < 0)
        y_start = 0;
    int32_t length = (max_y < H ? max_y : H) - y_start;
    if (length <= 0)
        return;

    int nl = 0, nr = 0;
    for (int side = 0; side < 2; side++)
    {
        int dir = side ? -left_dir : left_dir;
        int cur = side ? min_r : min_l;
        bool skip_first = !top_is_flat;
        int guard = count;
        do
        {
            int next = dir > 0 ? FWD(cur) : BACK(cur);
            if (side)
                nr = scan_edge(edge_right, nr, PX(cur), PY(cur), PX(next), PY(next), skip_first);
            else
                nl = scan_edge(edge_left, nl, PX(cur), PY(cur), PX(next), PY(next), skip_first);
            skip_first = false;
            cur = next;
        } while (cur != max_i && --guard > 0);
    }

    if (length > nl) length = nl;
    if (length > nr) length = nr;
    uint8_t *row = bm + y_start * pitch;
    for (int i = 0; i < length; i++, row += pitch)
    {
        int x0 = edge_left[i], x1 = edge_right[i];
        if (x0 < 0) x0 = 0;
        if (x1 > W - 1) x1 = W - 1;
        if (x1 >= x0)
            fill(row + x0, c, x1 - x0 + 1);
    }
#undef PX
#undef PY
#undef FWD
#undef BACK
}

void ir_raster_list(uint8_t *bm, int bm_pitch, const ir_dword_t *words, uint32_t used, ir_stats_t *stats)
{
    pitch = bm_pitch;
    uint32_t i = 0;
    while (i < used)
    {
        uint32_t header = (uint16_t)words[i++];
        int type = (header >> 12) & 0xF;
        int count = (header >> 6) & 0x3F;
        uint8_t color = (uint8_t)((header & 0x3F) + IR_PIXEL_BASE);
        int32_t pts[IR_MAX_POINTS * 2];
        for (int n = 0; n < count * 2; n++)
            pts[n] = words[i + n];
        i += (uint32_t)count * 2;

        switch (type)
        {
        case IR_CMD_ERASE:
            for (int y = 0; y < H; y++)
                fill(bm + y * pitch, IR_PIXEL_BASE, W);
            break;

        case IR_CMD_POLYGON:
            stats->polygons++;
            draw_polygon(bm, pts, count, color);
            break;

        case IR_CMD_VECTOR:
            stats->vectors++;
            // An open polyline: the math box microcode does not join the
            // last point back to the first (checked against harness
            // --reference). The shield lettering depends on it.
            for (int n = 0; n + 1 < count; n++)
                draw_line(bm, pts[n * 2], pts[n * 2 + 1], pts[n * 2 + 2], pts[n * 2 + 3], color);
            break;

        case IR_CMD_DOT:
            for (int n = 0; n < count; n++)
            {
                stats->dots++;
                plot(bm, pts[n * 2], pts[n * 2 + 1], color);
            }
            break;
        }
    }
}
