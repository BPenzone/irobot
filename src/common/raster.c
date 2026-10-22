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

static int16_t span_min[H], span_max[H];
static int pitch;

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

// Record the x extent of one polygon edge on each scanline it crosses.
// Spans cover pixel centers: scanline y is filled where the edge crosses
// y + 0.5, so shared edges between adjacent polygons are not drawn twice.
static void scan_edge(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int *ytop, int *ybot)
{
    if (y0 == y1)
        return;
    if (y0 > y1)
    {
        int32_t t;
        t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
    }

    int32_t ys = y0 < 0 ? 0 : y0;
    int32_t ye = y1 > H ? H : y1;   // exclusive
    if (ys >= ye)
        return;

    // x at scanline y = x0 + (y - y0) * dx / dy, in 16.16 fixed point
    int64_t dx = (int64_t)(x1 - x0) << 16;
    int64_t step = dx / (y1 - y0);
    int64_t x = ((int64_t)x0 << 16) + step * (ys - y0) + step / 2;

    if (ys < *ytop) *ytop = ys;
    if (ye > *ybot) *ybot = ye;

    for (int32_t y = ys; y < ye; y++, x += step)
    {
        int32_t xi = (int32_t)((x + 0x8000) >> 16);
        if (xi < -1) xi = -1;
        if (xi > W) xi = W;
        if (xi < span_min[y]) span_min[y] = (int16_t)xi;
        if (xi > span_max[y]) span_max[y] = (int16_t)xi;
    }
}

static void draw_polygon(uint8_t *bm, const int32_t *pts, int count, uint8_t c)
{
    int ytop = H, ybot = 0;

    for (int y = 0; y < H; y++)
    {
        span_min[y] = W + 1;
        span_max[y] = -2;
    }

    for (int i = 0; i < count; i++)
    {
        int j = (i + 1 == count) ? 0 : i + 1;
        scan_edge(pts[i * 2], pts[i * 2 + 1], pts[j * 2], pts[j * 2 + 1], &ytop, &ybot);
    }

    for (int y = ytop; y < ybot; y++)
    {
        int x0 = span_min[y], x1 = span_max[y];
        if (x0 < 0) x0 = 0;
        if (x1 > W) x1 = W;
        if (x1 > x0)
            memset(bm + y * pitch + x0, c, (size_t)(x1 - x0));
    }
}

void ir_raster_list(uint8_t *bm, int bm_pitch, const int32_t *words, uint32_t used, ir_stats_t *stats)
{
    pitch = bm_pitch;
    uint32_t i = 0;
    while (i < used)
    {
        uint32_t header = (uint32_t)words[i++];
        int type = (header >> 12) & 0xF;
        int count = (header >> 6) & 0x3F;
        uint8_t color = (uint8_t)((header & 0x3F) + IR_PIXEL_BASE);
        const int32_t *pts = &words[i];
        i += (uint32_t)count * 2;

        switch (type)
        {
        case IR_CMD_ERASE:
            for (int y = 0; y < H; y++)
                memset(bm + y * pitch, IR_PIXEL_BASE, W);
            break;

        case IR_CMD_POLYGON:
            stats->polygons++;
            draw_polygon(bm, pts, count, color);
            break;

        case IR_CMD_VECTOR:
            stats->vectors++;
            for (int n = 0; n + 1 < count; n++)
                draw_line(bm, pts[n * 2], pts[n * 2 + 1], pts[n * 2 + 2], pts[n * 2 + 3], color);
            // a polyline of three or more points is closed
            if (count > 2)
                draw_line(bm, pts[count * 2 - 2], pts[count * 2 - 1], pts[0], pts[1], color);
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
