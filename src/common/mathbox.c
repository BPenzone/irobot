// High-level math box: matrix commands, object rendering and terrain.
//
// Copyright 2026 the irobot 32X port authors.
// Adapted from John Manfreda's I, Robot Project emulator (1998 Mathbox.c,
// 3D.C, RenderOB.h and RenderPF.h, plus the 2020 Mathbox.cs and
// MathboxRenderer.cs), https://github.com/manfreda-dot-org/I-Robot-Project.
//
// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version. See LICENSE.
//
// All math is integer. Matrices and vectors are signed 16-bit with 14
// fractional bits, as on the original hardware. Instead of drawing directly,
// every primitive is projected to screen space and appended to the display
// list, which the rasterizer draws later in the same order.

#include "internal.h"

// Command words written to math RAM word 0 before MATH_START.
#define CMD_TERRAIN            0x8400
#define CMD_TERRAIN_REENTRANT  0x8600
#define CMD_ROLL               0x8800
#define CMD_YAW                0x9000
#define CMD_PITCH              0xA000
#define CMD_CONCATENATE        0xC000

// Special locations in math RAM (word addresses).
#define VIEW_POSITION   0x12    // camera X, Y, Z
#define VIEW_MATRIX     0x15    // default camera matrix
#define LIGHT_VECTOR    0x44
#define TERRAIN_OBJECTS 0x6B
#define TERRAIN_Z_MAX   0x6C
#define TERRAIN_Z_MIN   0x6D
#define TERRAIN_TILES   0x0E00
#define TERRAIN_MODE    0x72
#define TERRAIN_X       0x74
#define TERRAIN_Z_FRAC  0x75
#define TERRAIN_X_OFS   0x76
#define TERRAIN_Z_OFS   0x77

// The game sometimes points at this ROM address for the camera matrix,
// which the original emulator redirects to the RAM copy.
#define VIEW_MATRIX_ALIAS 0x787C

#define TILE_SIZE_X 128
#define TILE_SIZE_Y 64
#define TILE_SIZE_Z 128


static ir_stats_t *stats;

static int32_t cam_x, cam_y, cam_z;
static int32_t light_x, light_y, light_z;
static int32_t cam_matrix[9];
static int32_t rot_matrix[9];

static inline int32_t s16(uint16_t w) { return (int16_t)w; }
static inline int32_t imin(int32_t a, int32_t b) { return a < b ? a : b; }
static inline int32_t imax(int32_t a, int32_t b) { return a > b ? a : b; }

// ---------------------------------------------------------------------------
// Matrix commands. The math box stores matrices column by column, so these
// operate on the transposed layout exactly as the hardware does.
// ---------------------------------------------------------------------------

static void pitch(uint16_t *m, int32_t sn, int32_t cs)
{
    int32_t b = s16(m[1]), c = s16(m[2]);
    int32_t e = s16(m[4]), f = s16(m[5]);
    int32_t h = s16(m[7]), i = s16(m[8]);
    m[1] = (uint16_t)((b * cs - c * sn) >> 14);
    m[2] = (uint16_t)((c * cs + b * sn) >> 14);
    m[4] = (uint16_t)((e * cs - f * sn) >> 14);
    m[5] = (uint16_t)((f * cs + e * sn) >> 14);
    m[7] = (uint16_t)((h * cs - i * sn) >> 14);
    m[8] = (uint16_t)((i * cs + h * sn) >> 14);
}

static void yaw(uint16_t *m, int32_t sn, int32_t cs)
{
    int32_t a = s16(m[0]), c = s16(m[2]);
    int32_t d = s16(m[3]), f = s16(m[5]);
    int32_t g = s16(m[6]), i = s16(m[8]);
    m[0] = (uint16_t)((a * cs + c * sn) >> 14);
    m[2] = (uint16_t)((c * cs - a * sn) >> 14);
    m[3] = (uint16_t)((d * cs + f * sn) >> 14);
    m[5] = (uint16_t)((f * cs - d * sn) >> 14);
    m[6] = (uint16_t)((g * cs + i * sn) >> 14);
    m[8] = (uint16_t)((i * cs - g * sn) >> 14);
}

static void roll(uint16_t *m, int32_t sn, int32_t cs)
{
    int32_t a = s16(m[0]), b = s16(m[1]);
    int32_t d = s16(m[3]), e = s16(m[4]);
    int32_t g = s16(m[6]), h = s16(m[7]);
    m[0] = (uint16_t)((a * cs - b * sn) >> 14);
    m[1] = (uint16_t)((b * cs + a * sn) >> 14);
    m[3] = (uint16_t)((d * cs - e * sn) >> 14);
    m[4] = (uint16_t)((e * cs + d * sn) >> 14);
    m[6] = (uint16_t)((g * cs - h * sn) >> 14);
    m[7] = (uint16_t)((h * cs + g * sn) >> 14);
}

// C = transpose(A x B). Results are stored one element at a time, as the
// reference emulators do, in case C overlaps A or B.
static void concatenate(const uint16_t *a, const uint16_t *b, uint16_t *c)
{
    for (int row = 0; row < 3; row++)
        for (int col = 0; col < 3; col++)
            c[row * 3 + col] = (uint16_t)((s16(a[row * 3 + 0]) * s16(b[col * 3 + 0]) +
                                           s16(a[row * 3 + 1]) * s16(b[col * 3 + 1]) +
                                           s16(a[row * 3 + 2]) * s16(b[col * 3 + 2])) >> 14);
}

// ---------------------------------------------------------------------------
// Shared 3D helpers
// ---------------------------------------------------------------------------

static void load_matrix(int32_t *dst, uint16_t addr)
{
    if (addr == VIEW_MATRIX_ALIAS)
        addr = VIEW_MATRIX;
    for (int n = 0; n < 9; n++)
        dst[n] = s16(M((addr + n) & 0x7FFF));
}

static void get_camera(void)
{
    light_x = s16(M(LIGHT_VECTOR + 0));
    light_y = s16(M(LIGHT_VECTOR + 1));
    light_z = s16(M(LIGHT_VECTOR + 2));
    cam_x = s16(M(VIEW_POSITION + 0));
    cam_y = s16(M(VIEW_POSITION + 1));
    cam_z = s16(M(VIEW_POSITION + 2));
}

static inline void rotate(const int32_t *m, int32_t *x, int32_t *y, int32_t *z)
{
    int32_t ox = *x, oy = *y, oz = *z;
    *x = (ox * m[0] + oy * m[1] + oz * m[2]) >> 14;
    *y = (ox * m[3] + oy * m[4] + oz * m[5]) >> 14;
    *z = (ox * m[6] + oy * m[7] + oz * m[8]) >> 14;
}

// Perspective projection to native screen coordinates. Like the original,
// a point at z == 0 is left unprojected.
static inline void project(int32_t *x, int32_t *y, int32_t z)
{
    if (z)
    {
        *x = (*x << 8) / z;
        *y = (*y << 8) / z;
    }
    *x += IR_XMID;
    *y += IR_YMID;
}

// ---------------------------------------------------------------------------
// Objects
// ---------------------------------------------------------------------------

// Object being drawn: its translation and its vertex table.
static int32_t obj_dx, obj_dy, obj_dz;
static uint16_t vertex_table;

// Fetch a vertex by its instruction word, rotated and translated.
static void fetch_point(uint16_t insn, int32_t *x, int32_t *y, int32_t *z)
{
    uint16_t a = (uint16_t)(vertex_table + (insn & 0x3FFF));
    *x = s16(M(a & 0x7FFF));
    *y = s16(M((a + 1) & 0x7FFF));
    *z = s16(M((a + 2) & 0x7FFF));
    rotate(rot_matrix, x, y, z);
    *x += obj_dx;
    *y += obj_dy;
    *z += obj_dz;
}

// Draw one primitive. Returns whether its face is visible; primitives
// without a normal vector always count as visible.
static bool render_primitive(uint16_t addr, uint16_t control)
{
    int32_t pts[2 * (IR_MAX_POINTS + 1)];
    int count = 0;
    int color = control & 0x3F;
    int32_t x, y, z;

    uint16_t insn = M(addr++ & 0x7FFF);
    if (!(insn & 0x4000))
    {
        // First word is a normal vector; rotate it (no translation).
        uint16_t a = (uint16_t)(vertex_table + (insn & 0x3FFF));
        int32_t nx = s16(M(a & 0x7FFF));
        int32_t ny = s16(M((a + 1) & 0x7FFF));
        int32_t nz = s16(M((a + 2) & 0x7FFF));
        rotate(rot_matrix, &nx, &ny, &nz);

        insn = M(addr++ & 0x7FFF);
        fetch_point(insn, &x, &y, &z);
        if (nx * x + ny * y + nz * z <= 0)
            return false;

        if (control & 0x0040)
            color += imin(7, imax(0, (nx * light_x + ny * light_y + nz * light_z) >> 25));
    }
    else
    {
        // Without a normal vector the first word is not drawn: the
        // vertices start with the next word (checked against the math box
        // microcode with harness --reference; the shield lettering shows it).
        if (insn & 0x8000)
            return true;
        insn = M(addr++ & 0x7FFF);
        fetch_point(insn, &x, &y, &z);
    }

    // Branch-only entries test visibility without drawing.
    if (control & 0x3000)
        return true;

    for (;;)
    {
        project(&x, &y, z);
        if (count <= IR_MAX_POINTS)
        {
            pts[count * 2] = x;
            pts[count * 2 + 1] = y;
            count++;
        }
        if (insn & 0x8000)
            break;
        insn = M(addr++ & 0x7FFF);
        fetch_point(insn, &x, &y, &z);
    }
    if (count > IR_MAX_POINTS)
        count = IR_MAX_POINTS;

    color &= 0x3F;
    switch (control & 0x0300)
    {
    case 0x0000:
        if (count > 2)
            ir_dlist_add(IR_CMD_POLYGON, color, pts, count);
        else if (count == 2)
            ir_dlist_add(IR_CMD_VECTOR, color, pts, 2);
        else
            ir_dlist_add(IR_CMD_DOT, color, pts, 1);
        break;
    case 0x0100:
        if (count > 1)
            ir_dlist_add(IR_CMD_VECTOR, color, pts, count);
        else
            ir_dlist_add(IR_CMD_DOT, color, pts, 1);
        break;
    default:
        ir_dlist_add(IR_CMD_DOT, color, pts, count);
        break;
    }
    return true;
}

// Walk a primitive list. The first word is the vertex table address, then
// pairs of (primitive address, control word) until an address >= 0x8000.
static void render_primitive_list(uint16_t addr)
{
    vertex_table = M(addr++ & 0x7FFF);

    for (int guard = 0; guard < 4096; guard++)
    {
        uint16_t prim = M(addr++ & 0x7FFF);
        if (prim >= 0x8000)
            return;
        uint16_t control = M(addr++ & 0x7FFF);
        bool visible = render_primitive(prim, control);

        // Bit 15 marks a branch over hidden-surface groups:
        // 0x1000 = branch if visible, 0x2000 = branch if hidden,
        // otherwise always. Bit 14 selects an absolute target.
        if (control & 0x8000)
        {
            bool branch = true;
            switch (control & 0x3000)
            {
            case 0x1000: branch = visible; break;
            case 0x2000: branch = !visible; break;
            }
            if (!branch)
                addr++;
            else if (control & 0x4000)
                addr = M(addr & 0x7FFF);
            else
                addr = (uint16_t)(addr + M(addr & 0x7FFF));
        }
    }
}

// Render an object and its children. Object record layout:
//   +0..+2  position X, Y, Z
//   +3      control: 0x4000 keep camera matrix, 0x0800 reuse previous
//           primitives and rotation, 0x0400 position is relative
//   +4      camera matrix address
//   +5, +6  rotation matrix and primitive list (unless 0x0800)
//   then    child object addresses; 0x0002 continues with the next
//           record 8 words on, 0 or >= 0x8000 ends the list.
static void render_object(uint16_t addr, int depth)
{
    static uint16_t prim_list;

    if (depth > 32)
        return;

    for (;;)
    {
        if (addr == 0 || addr >= 0x8000)
            return;
        stats->objects++;

        uint16_t control = M(addr + 3);
        if (!(control & 0x4000))
            load_matrix(cam_matrix, M((addr + 4) & 0x7FFF));

        int child = 5;
        if (!(control & 0x0800))
        {
            prim_list = M((addr + 6) & 0x7FFF);
            if (prim_list >= 0x8000)
                return;
            load_matrix(rot_matrix, M((addr + 5) & 0x7FFF));
            child = 7;
        }
        if (prim_list >= 0x8000)
            return;

        int32_t x = s16(M(addr)), y = s16(M((addr + 1) & 0x7FFF)), z = s16(M((addr + 2) & 0x7FFF));
        if (control & 0x0400)
        {
            rotate(cam_matrix, &x, &y, &z);
            obj_dx += x;
            obj_dy += y;
            obj_dz += z;
        }
        else
        {
            x -= cam_x;
            y -= cam_y;
            z -= cam_z;
            rotate(cam_matrix, &x, &y, &z);
            obj_dx = x;
            obj_dy = y;
            obj_dz = z;
        }

        render_primitive_list(prim_list);

        for (;;)
        {
            uint16_t word = M((addr + child++) & 0x7FFF);
            if (word == 0 || word >= 0x8000)
                return;
            if (word == 0x0002)
            {
                addr += 8;
                break;
            }
            render_object(word, depth + 1);
        }
    }
}

// ---------------------------------------------------------------------------
// Terrain (the "playfield" of tiles the robot walks on)
// ---------------------------------------------------------------------------

static uint16_t terrain_objects, terrain_mode;
static int terrain_rows_left, left_of_camera, right_of_camera;
static int32_t xv[3], yv[3], zv[3];

// Neighbors of the tile being drawn (A is the current tile):
//          C   D
//      F   A   B
//          E
static uint16_t tile_a, tile_b, tile_c, tile_d, tile_e, tile_f;

static void tile_output(const int32_t *pts, int color, uint16_t mode)
{
    color &= 0x3F;
    if (mode == 0x0000)
        ir_dlist_add(IR_CMD_POLYGON, color, pts, 4);
    else if (mode == 0x0100)
        ir_dlist_add(IR_CMD_VECTOR, color, pts, 4);
    else
        ir_dlist_add(IR_CMD_DOT, color, pts, 4);
}

typedef struct { int32_t x, y, z; bool done; } tpoint_t;

static inline void tproject(tpoint_t *p)
{
    if (!p->done)
    {
        if (p->z)
        {
            p->x = (p->x << 8) / p->z + IR_XMID;
            p->y = (p->y << 8) / p->z + IR_YMID;
        }
        else
        {
            p->x += IR_XMID;
            p->y += IR_YMID;
        }
        p->done = true;
    }
}

static inline tpoint_t tp(int32_t x, int32_t y, int32_t z)
{
    tpoint_t p = { x, y, z, false };
    return p;
}

static inline void tile_height_offset(tpoint_t *p, int32_t h)
{
    p->x += (h * yv[0]) >> 4;
    p->y += (h * yv[1]) >> 4;
    p->z += (h * yv[2]) >> 4;
}

static inline int32_t tile_height(uint16_t t)
{
    return (t & 0x7F00) ? ((int16_t)t) >> 8 : 0;
}

static inline bool tile_empty(uint16_t t)
{
    return (t >> 8) == 0x80;
}

static void draw_tile(int32_t ax_, int32_t ay_, int32_t az_, int32_t bx_, int32_t by_, int32_t bz_)
{
    int32_t pts[8];

    if (tile_empty(tile_a) || az_ <= 0 || bz_ <= 0)
        goto objects;

    // Top corners: a (front left), c (front right), e (back left), g (back right).
    // Bottom corners b, d, f, h follow the same pattern.
    tpoint_t a = tp(ax_, ay_, az_);
    tpoint_t c = tp(ax_ + xv[0], ay_ + xv[1], az_ + xv[2]);
    tpoint_t e = tp(ax_ + zv[0], ay_ + zv[1], az_ + zv[2]);
    tpoint_t g = tp(c.x + zv[0], c.y + zv[1], c.z + zv[2]);

    if (tile_a & 0x40)
    {
        // Flat tile, drawn as a box.
        int color = tile_a & 0x3F;
        int32_t height_a = 0;
        if (tile_a & 0x7F00)
        {
            height_a = ((int16_t)tile_a) >> 8;
            tile_height_offset(&a, height_a);
            tile_height_offset(&c, height_a);
            tile_height_offset(&e, height_a);
            tile_height_offset(&g, height_a);
        }

        tpoint_t b = tp(bx_, by_, bz_);
        tpoint_t d = tp(bx_ + xv[0], by_ + xv[1], bz_ + xv[2]);
        tpoint_t f = tp(bx_ + zv[0], by_ + zv[1], bz_ + zv[2]);
        tpoint_t h = tp(d.x + zv[0], d.y + zv[1], d.z + zv[2]);

        tproject(&a);
        tproject(&e);

        // Only one of the left or right sides can face the camera.
        if (a.x > e.x)
        {
            if (tile_height(tile_f) > height_a || tile_empty(tile_f))
            {
                tproject(&b);
                tproject(&f);
                int32_t q[8] = { e.x, e.y, f.x, f.y, b.x, b.y, a.x, a.y };
                tile_output(q, color - 2, terrain_mode);
            }
        }
        else
        {
            tproject(&c);
            tproject(&g);
            if (c.x < g.x)
            {
                if (tile_height(tile_b) > height_a || tile_empty(tile_b))
                {
                    tproject(&d);
                    tproject(&h);
                    int32_t q[8] = { c.x, c.y, d.x, d.y, h.x, h.y, g.x, g.y };
                    tile_output(q, color - 2, terrain_mode);
                }
            }
        }

        // Front face: last row, or the tile in front is lower or empty.
        if (!terrain_rows_left || tile_height(tile_e) > height_a || tile_empty(tile_e))
        {
            tproject(&b);
            tproject(&c);
            tproject(&d);
            int32_t q[8] = { a.x, a.y, b.x, b.y, d.x, d.y, c.x, c.y };
            tile_output(q, color - 4, terrain_mode);
        }

        // Top face, if it faces the camera.
        if (a.y > e.y)
        {
            tproject(&c);
            tproject(&g);
            int32_t q[8] = { e.x, e.y, a.x, a.y, c.x, c.y, g.x, g.y };
            tile_output(q, color, terrain_mode);
        }
    }
    else
    {
        // Sloped tile: each top corner takes the height of its own tile.
        if (tile_a & 0x7F00) tile_height_offset(&a, ((int16_t)tile_a) >> 8);
        if (tile_b & 0x7F00) tile_height_offset(&c, ((int16_t)tile_b) >> 8);
        if (tile_c & 0x7F00) tile_height_offset(&e, ((int16_t)tile_c) >> 8);
        if (tile_d & 0x7F00) tile_height_offset(&g, ((int16_t)tile_d) >> 8);

        tproject(&a);
        tproject(&c);
        tproject(&e);
        tproject(&g);
        pts[0] = e.x; pts[1] = e.y;
        pts[2] = a.x; pts[3] = a.y;
        pts[4] = c.x; pts[5] = c.y;
        pts[6] = g.x; pts[7] = g.y;

        // A solid slope seen from below is drawn as an outline, which is
        // how the original hardware appears to behave.
        uint16_t mode = terrain_mode;
        if (mode == 0x0000)
        {
            if (g.x < IR_XMID)
                mode = (g.y > c.y) ? 0x0100 : 0x0000;
            else if (e.x > IR_XMID)
                mode = (e.y > a.y) ? 0x0100 : 0x0000;
            else
                mode = (e.y > a.y || g.y > c.y) ? 0x0100 : 0x0000;
        }
        tile_output(pts, tile_a & 0x3F, mode);
    }

objects:
    // Objects standing on this tile are drawn right after it.
    if (tile_a & 0x80)
    {
        int count = M(terrain_objects++ & 0x7FFF) + 1;
        while (count-- > 0)
            render_object(M(terrain_objects++ & 0x7FFF), 0);
    }
}

static void draw_terrain_row(int row,
    int32_t tlx, int32_t tly, int32_t tlz,
    int32_t trx, int32_t try_, int32_t trz,
    int32_t blx, int32_t bly, int32_t blz,
    int32_t brx, int32_t bry, int32_t brz)
{
    const uint16_t *next = &M(TERRAIN_TILES + (((row + 1) & 31) << 4));
    const uint16_t *cur  = &M(TERRAIN_TILES + (row << 4));
    const uint16_t *prev = &M(TERRAIN_TILES + (((row + 31) & 31) << 4));

    // Left edge towards the middle, not including the middle.
    if (left_of_camera > 0)
    {
        int c = left_of_camera;
        int n = 2;
        tile_a = cur[1];
        tile_c = next[1];
        tile_e = prev[1];
        tile_f = cur[0];
        while (n < 16 && --c)
        {
            tile_b = cur[n];
            tile_d = next[n];
            draw_tile(tlx, tly, tlz, blx, bly, blz);
            tile_e = prev[n];
            tile_f = tile_a;
            tile_a = tile_b;
            tile_c = tile_d;
            tlx += xv[0]; blx += xv[0];
            tly += xv[1]; bly += xv[1];
            tlz += xv[2]; blz += xv[2];
            n++;
        }
    }

    // Right edge towards the middle, including the middle.
    if (right_of_camera > 0)
    {
        int c = right_of_camera;
        int n = 14;
        tile_a = cur[15];
        tile_b = cur[0];
        tile_c = next[15];
        tile_d = next[0];
        tile_e = prev[15];
        while (n >= 0 && c--)
        {
            tile_f = cur[n];
            draw_tile(trx, try_, trz, brx, bry, brz);
            tile_b = tile_a;
            tile_a = tile_f;
            tile_d = tile_c;
            tile_c = next[n];
            tile_e = prev[n];
            trx -= xv[0]; brx -= xv[0];
            try_ -= xv[1]; bry -= xv[1];
            trz -= xv[2]; brz -= xv[2];
            n--;
        }
    }
}

static void render_terrain(void)
{
    stats->terrains++;

    terrain_mode = M(TERRAIN_MODE);
    terrain_objects = M(TERRAIN_OBJECTS);
    terrain_rows_left = s16(M(TERRAIN_Z_MAX)) - s16(M(TERRAIN_Z_MIN)) + 1;

    left_of_camera = cam_x >> 7;
    right_of_camera = 16 - left_of_camera;

    int row = (M(TERRAIN_Z_OFS) >> 4) + M(TERRAIN_Z_MAX) + 1024;

    load_matrix(cam_matrix, VIEW_MATRIX);
    const int32_t *m = cam_matrix;

    xv[0] = (TILE_SIZE_X * m[0]) >> 14;
    xv[1] = (TILE_SIZE_X * m[3]) >> 14;
    xv[2] = (TILE_SIZE_X * m[6]) >> 14;
    yv[0] = (TILE_SIZE_Y * m[1]) >> 14;
    yv[1] = (TILE_SIZE_Y * m[4]) >> 14;
    yv[2] = (TILE_SIZE_Y * m[7]) >> 14;
    zv[0] = (TILE_SIZE_Z * m[2]) >> 14;
    zv[1] = (TILE_SIZE_Z * m[5]) >> 14;
    zv[2] = (TILE_SIZE_Z * m[8]) >> 14;

    int32_t x = TILE_SIZE_X - (s16(M(TERRAIN_X_OFS)) << 7) - s16(M(TERRAIN_X));
    int32_t y = -cam_y;
    int32_t z = (s16(M(TERRAIN_Z_MAX)) << 7) - s16(M(TERRAIN_Z_FRAC));

    int32_t tl[3], tr[3], bl[3], br[3];
    tl[0] = (x * m[0] + y * m[1] + z * m[2]) >> 14;
    tl[1] = (x * m[3] + y * m[4] + z * m[5]) >> 14;
    tl[2] = (x * m[6] + y * m[7] + z * m[8]) >> 14;
    for (int i = 0; i < 3; i++)
    {
        tr[i] = tl[i] + xv[i] * 14;
        bl[i] = tl[i] + (yv[i] << 2);
        br[i] = tr[i] + (yv[i] << 2);
    }

    while (terrain_rows_left-- > 0)
    {
        draw_terrain_row(row-- & 31,
            tl[0], tl[1], tl[2], tr[0], tr[1], tr[2],
            bl[0], bl[1], bl[2], br[0], br[1], br[2]);
        for (int i = 0; i < 3; i++)
        {
            tl[i] -= zv[i];
            tr[i] -= zv[i];
            bl[i] -= zv[i];
            br[i] -= zv[i];
        }
    }
}

// ---------------------------------------------------------------------------

void ir_mathbox_start(ir_stats_t *s)
{
    stats = s;
    stats->mathbox_commands++;

    uint16_t command = M(0);
    switch (command)
    {
    case CMD_TERRAIN:
        get_camera();
        render_terrain();
        break;
    case CMD_TERRAIN_REENTRANT:
        break;
    case CMD_ROLL:
        roll(&M(M(6) & 0x7FFF), s16(M(7)), s16(M(8)));
        break;
    case CMD_YAW:
        yaw(&M(M(6) & 0x7FFF), s16(M(7)), s16(M(8)));
        break;
    case CMD_PITCH:
        pitch(&M(M(6) & 0x7FFF), s16(M(7)), s16(M(8)));
        break;
    case CMD_CONCATENATE:
        concatenate(&M(M(0x7B) & 0x7FFF), &M(M(0x7C) & 0x7FFF), &M(M(0x7D) & 0x7FFF));
        break;
    default:
        if (command < 0x8000)
        {
            get_camera();
            render_object(command, 0);
        }
        break;
    }
}
