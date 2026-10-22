// Reference math box and video processor for the PC harness (--reference).
//
// The game normally runs with the high-level math box in src/common/mathbox.c,
// which skips the math box microcode and draws primitives directly. This file
// instead runs the real microcode from the 136029-111 to -123 PROMs and draws
// the display list it leaves in COM RAM the way the video processor does, so
// the high-level version has something exact to be compared with.
//
// Ported from MAME's I, Robot driver (src/mame/atari/irobot_m.cpp and
// irobot_v.cpp), license BSD-3-Clause, copyright holders Dan Boris and the
// MAME team.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/common/internal.h"
#include "reference.h"

enum
{
    FL_MULT = 0x01,
    FL_SHIFT = 0x02,
    FL_MBMEMDEC = 0x04,
    FL_ADDEN = 0x08,
    FL_DPSEL = 0x10,
    FL_CARRY = 0x20,
    FL_DIV = 0x40,
    FL_MBRW = 0x80,
};

typedef struct irmb_ops
{
    const struct irmb_ops *nxtop;
    uint32_t func;
    uint32_t diradd;
    uint32_t latchmask;
    uint32_t *areg;
    uint32_t *breg;
    uint8_t cycles;
    uint8_t diren;
    uint8_t flags;
    uint8_t ramsel;
} irmb_ops;

static irmb_ops mbops[1024];
static const irmb_ops *irmb_stack[16];
static uint32_t irmb_regs[16];
static uint32_t irmb_latch;

static uint16_t *math_ram;          // 0x1000 words
static const uint8_t *math_rom_be;  // 0x6000 words, high byte first
static uint8_t *comram;             // two banks of 0x2000 bytes, high byte first
static int commbank;

static uint8_t polybitmap[2][256 * IR_SCREEN_H];
static int bufsel, vg_clear, statwr_old;

static uint16_t rom_word(uint32_t n)
{
    return (uint16_t)(math_rom_be[n * 2] << 8 | math_rom_be[n * 2 + 1]);
}

static uint16_t com_read(int bank, uint32_t n)
{
    const uint8_t *p = comram + bank * 0x2000 + (n & 0xFFF) * 2;
    return (uint16_t)(p[0] << 8 | p[1]);
}

static void com_write(int bank, uint32_t n, uint16_t v)
{
    uint8_t *p = comram + bank * 0x2000 + (n & 0xFFF) * 2;
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint32_t irmb_din(const irmb_ops *curop)
{
    uint32_t d = 0;
    if (!(curop->flags & FL_MBMEMDEC) && (curop->flags & FL_MBRW))
    {
        uint32_t ad = curop->diradd | (irmb_latch & curop->latchmask);
        if (curop->diren || (irmb_latch & 0x6000) == 0)
            d = math_ram[ad & 0xfff];
        else if (irmb_latch & 0x4000)
            d = rom_word(ad + 0x2000);
        else
            d = rom_word(ad & 0x1fff);
    }
    return d;
}

static void irmb_dout(const irmb_ops *curop, uint32_t d)
{
    if (curop->ramsel == 3)
        com_write(commbank ^ 1, irmb_latch & 0x7ff, (uint16_t)d);
    if (!(curop->flags & FL_MBMEMDEC))
    {
        uint32_t ad = curop->diradd | (irmb_latch & curop->latchmask);
        if (curop->diren || (irmb_latch & 0x6000) == 0)
            math_ram[ad & 0xfff] = (uint16_t)d;
    }
}

void ref_init(const uint8_t *proms)
{
    const uint8_t *MB = proms;   // 136029-111 .. -123, 0x400 bytes each
    for (int i = 0; i < 1024; i++)
    {
        int nxtadd, func, ramsel, diradd, latchmask, dirmask, time;

        mbops[i].areg = &irmb_regs[MB[0x0000 + i] & 0x0f];
        mbops[i].breg = &irmb_regs[MB[0x0400 + i] & 0x0f];
        func = (MB[0x0800 + i] & 0x0f) << 5;
        func |= ((MB[0x0c00 + i] & 0x0f) << 1);
        func |= (MB[0x1000 + i] & 0x08) >> 3;
        time = MB[0x1000 + i] & 0x03;
        mbops[i].flags = (MB[0x1000 + i] & 0x04) >> 2;
        nxtadd = (MB[0x1400 + i] & 0x0c) >> 2;
        diradd = MB[0x1400 + i] & 0x03;
        nxtadd |= ((MB[0x1800 + i] & 0x0f) << 6);
        nxtadd |= ((MB[0x1c00 + i] & 0x0f) << 2);
        diradd |= (MB[0x2000 + i] & 0x0f) << 2;
        func |= (MB[0x2400 + i] & 0x0e) << 9;
        mbops[i].flags |= (MB[0x2400 + i] & 0x01) << 1;
        mbops[i].flags |= (MB[0x2800 + i] & 0x0f) << 2;
        mbops[i].flags |= ((MB[0x2c00 + i] & 0x01) << 6);
        mbops[i].flags |= (MB[0x2c00 + i] & 0x08) << 4;
        ramsel = (MB[0x2c00 + i] & 0x06) >> 1;
        diradd |= (MB[0x3000 + i] & 0x03) << 6;

        if (mbops[i].flags & FL_SHIFT)
            func |= 0x200;

        mbops[i].func = (uint32_t)func;
        mbops[i].nxtop = &mbops[nxtadd];
        mbops[i].cycles = (uint8_t)(time == 3 ? 2 : 3 + time);

        if (ramsel == 0)
        {
            dirmask = 0x00fc;
            latchmask = 0x3000;
        }
        else
        {
            dirmask = 0x0000;
            latchmask = 0x3ffc;
        }
        if (ramsel & 2)
            latchmask |= 0x0003;
        else
            dirmask |= 0x0003;

        mbops[i].ramsel = (uint8_t)ramsel;
        mbops[i].diradd = (uint32_t)(diradd & dirmask);
        mbops[i].latchmask = (uint32_t)latchmask;
        mbops[i].diren = ramsel == 0;
    }
    for (int i = 0; i < 16; i++)
        irmb_stack[i] = &mbops[0];
}

#define COMPUTE_CI \
    CI = 0; \
    if (curop->flags & FL_DPSEL) \
        CI = cflag; \
    else \
    { \
        if (curop->flags & FL_CARRY) \
            CI = 1; \
        if (!(prevop->flags & FL_DIV) && !nflag) \
            CI = 1; \
    }

#define ADD(r, s) \
    COMPUTE_CI; \
    result = r + s + CI; \
    cflag = (result >> 16) & 1; \
    vflag = (((r & 0x7fff) + (s & 0x7fff) + CI) >> 15) ^ cflag

#define SUBR(r, s) \
    COMPUTE_CI; \
    result = (r ^ 0xffff) + s + CI; \
    cflag = (result >> 16) & 1; \
    vflag = (((s & 0x7fff) + ((r ^ 0xffff) & 0x7fff) + CI) >> 15) ^ cflag

#define SUB(r, s) \
    COMPUTE_CI; \
    result = r + (s ^ 0xffff) + CI; \
    cflag = (result >> 16) & 1; \
    vflag = (((r & 0x7fff) + ((s ^ 0xffff) & 0x7fff) + CI) >> 15) ^ cflag

#define OR(r, s)   result = r | s; vflag = cflag = 0
#define AND(r, s)  result = r & s; vflag = cflag = 0
#define IAND(r, s) result = (r ^ 0xffff) & s; vflag = cflag = 0
#define XOR(r, s)  result = r ^ s; vflag = cflag = 0
#define IXOR(r, s) result = (r ^ s) ^ 0xffff; vflag = cflag = 0

static void irmb_run(void)
{
    const irmb_ops *prevop = &mbops[0];
    const irmb_ops *curop = &mbops[0];

    uint32_t Q = 0, Y = 0, nflag = 0, vflag = 0, cflag = 0, zresult = 1, CI = 0, SP = 0;
    long guard = 0;

    while ((prevop->flags & (FL_DPSEL | FL_CARRY)) != (FL_DPSEL | FL_CARRY) && ++guard < 50000000)
    {
        uint32_t result = 0;
        uint32_t fu = curop->func;

        if (!(prevop->flags & FL_MULT) || (Q & 1))
            fu = fu ^ 0x02;
        else
            fu = fu | 0x02;

        if ((prevop->flags & FL_DIV) || nflag)
            fu = fu ^ 0x08;
        else
            fu = fu | 0x08;

        // Source operands R and S (bits 0-2) and the ALU function (bits 3-5),
        // as in the 2901 slices.
        uint32_t A = *curop->areg, B = *curop->breg;
        uint32_t R, S;
        switch (fu & 0x07)
        {
        case 0: R = A; S = Q; break;
        case 1: R = A; S = B; break;
        case 2: R = 0; S = Q; break;
        case 3: R = 0; S = B; break;
        case 4: R = 0; S = A; break;
        case 5: R = irmb_din(curop); S = A; break;
        case 6: R = irmb_din(curop); S = Q; break;
        default: R = irmb_din(curop); S = 0; break;
        }
        switch ((fu >> 3) & 0x07)
        {
        case 0: ADD(R, S); break;
        case 1: SUBR(R, S); break;
        case 2: SUB(R, S); break;
        case 3: OR(R, S); break;
        case 4: AND(R, S); break;
        case 5: IAND(R, S); break;
        case 6: XOR(R, S); break;
        default: IXOR(R, S); break;
        }

        zresult = result & 0xffff;
        nflag = zresult >> 15;
        prevop = curop;

        // Destination (bits 6-8, with bit 9 selecting the shift variant)
        int dest = (fu >> 6) & 0x07;
        int shift = (fu >> 9) & 1;
        switch (dest)
        {
        case 0: Q = Y = zresult; break;
        case 1: Y = zresult; break;
        case 2: Y = *curop->areg; *curop->breg = zresult; break;
        case 3: *curop->breg = zresult; Y = zresult; break;
        case 4:
            if (shift)
            {
                *curop->breg = (zresult >> 1) | ((nflag ^ vflag) << 15);
                Q = (Q >> 1) | ((zresult & 0x01) << 15);
            }
            else
            {
                *curop->breg = (zresult >> 1) | ((curop->flags & 0x20) << 10);
                Q = (Q >> 1) | ((curop->flags & 0x20) << 10);
            }
            Y = zresult;
            break;
        case 5:
            if (shift)
                *curop->breg = (zresult >> 1) | ((nflag ^ vflag) << 15);
            else
                *curop->breg = (zresult >> 1) | ((curop->flags & 0x20) << 10);
            Y = zresult;
            break;
        case 6:
            if (shift)
            {
                *curop->breg = (zresult << 1) | ((Q & 0x8000) >> 15);
                Q = (Q << 1) & 0xffff;
            }
            else
            {
                *curop->breg = zresult << 1;
                Q = ((Q << 1) & 0xffff) | (nflag ^ 1);
            }
            Y = zresult;
            break;
        default:
            if (shift)
                *curop->breg = (zresult << 1) | ((Q & 0x8000) >> 15);
            else
                *curop->breg = zresult << 1;
            Y = zresult;
            break;
        }

        // Jump (bits 10-12)
        switch ((fu >> 10) & 0x07)
        {
        case 0: curop++; break;
        case 1: curop = cflag ? curop->nxtop : curop + 1; break;
        case 2: curop = !zresult ? curop->nxtop : curop + 1; break;
        case 3: curop = !nflag ? curop->nxtop : curop + 1; break;
        case 4: curop = nflag ? curop->nxtop : curop + 1; break;
        case 5: curop = curop->nxtop; break;
        case 6: irmb_stack[SP] = curop + 1; SP = (SP + 1) & 15; curop = curop->nxtop; break;
        default: SP = (SP - 1) & 15; curop = irmb_stack[SP]; break;
        }

        if (!(prevop->flags & FL_MBRW))
            irmb_dout(prevop, Y);

        if (!(prevop->flags & FL_ADDEN))
        {
            if (prevop->flags & FL_MBRW)
                irmb_latch = irmb_din(prevop);
            else
                irmb_latch = Y;
        }
    }
    if (guard >= 50000000)
        fprintf(stderr, "reference math box did not finish\n");
}

// ---------------------------------------------------------------------------
// Video processor
// ---------------------------------------------------------------------------

#define XMAX 256
#define YMAX IR_SCREEN_H
#define DRAW_PIXEL(x, y, c) bm[(y) * 256 + (x)] = (uint8_t)(c)

static int round_to_pixel(int x) { return (x >> 7) - 128; }

static void draw_line(uint8_t *bm, int x1, int y1, int x2, int y2, int col)
{
    int dx = abs(x1 - x2), dy = abs(y1 - y2);
    int sx = (x1 <= x2) ? 1 : -1, sy = (y1 <= y2) ? 1 : -1;
    int cx = dx / 2, cy = dy / 2;

    if (dx >= dy)
    {
        for (;;)
        {
            if (x1 >= 0 && x1 < XMAX && y1 >= 0 && y1 < YMAX)
                DRAW_PIXEL(x1, y1, col);
            if (x1 == x2)
                break;
            x1 += sx;
            cx -= dy;
            if (cx < 0)
            {
                y1 += sy;
                cx += dx;
            }
        }
    }
    else
    {
        for (;;)
        {
            if (x1 >= 0 && x1 < XMAX && y1 >= 0 && y1 < YMAX)
                DRAW_PIXEL(x1, y1, col);
            if (y1 == y2)
                break;
            y1 += sy;
            cy -= dx;
            if (cy < 0)
            {
                x1 += sx;
                cy += dy;
            }
        }
    }
}

static void run_video(void)
{
    uint8_t *bm = polybitmap[bufsel];
    int bank = commbank;
    int lpnt = 0;
    while (lpnt < 0x7ff)
    {
        uint32_t d1 = com_read(bank, (uint32_t)lpnt++);
        if (d1 == 0xffff)
            break;
        int spnt = d1 & 0x07ff;
        int shp = (d1 & 0xf000) >> 12;

        if (shp == 0x8)
        {
            while (spnt < 0x7ff)
            {
                int sx = com_read(bank, (uint32_t)spnt);
                if (sx == 0xffff)
                    break;
                int sy = com_read(bank, (uint32_t)spnt + 1);
                int color = sy & 0x3f;
                sx = round_to_pixel(sx);
                sy = round_to_pixel(sy);
                if (sx >= 0 && sx < XMAX && sy >= 0 && sy < YMAX)
                    DRAW_PIXEL(sx, sy, color);
                spnt += 2;
            }
        }

        if (shp == 0xc)
        {
            while (spnt < 0x7ff)
            {
                int ey = com_read(bank, (uint32_t)spnt);
                if (ey == 0xffff)
                    break;
                ey = round_to_pixel(ey);
                int sy = com_read(bank, (uint32_t)spnt + 1);
                int color = sy & 0x3f;
                sy = round_to_pixel(sy);
                int sx = com_read(bank, (uint32_t)spnt + 3);
                int32_t word1 = (int16_t)com_read(bank, (uint32_t)spnt + 2);
                int ex = sx + word1 * (ey - sy + 1);
                draw_line(bm, round_to_pixel(sx), sy, round_to_pixel(ex), ey, color);
                spnt += 4;
            }
        }

        if (shp == 0x4)
        {
            int spnt2 = com_read(bank, (uint32_t)spnt) & 0x7ff;
            int sx = com_read(bank, (uint32_t)spnt + 1);
            int sx2 = com_read(bank, (uint32_t)spnt + 2);
            int sy = com_read(bank, (uint32_t)spnt + 3);
            int color = sy & 0x3f;
            sy = round_to_pixel(sy);
            spnt += 4;

            int32_t word1 = (int16_t)com_read(bank, (uint32_t)spnt);
            int ey = com_read(bank, (uint32_t)spnt + 1);
            if (word1 != -1 || ey != 0xffff)
            {
                ey = round_to_pixel(ey);
                spnt += 2;

                int32_t word2 = (int16_t)com_read(bank, (uint32_t)spnt2);
                int ey2 = round_to_pixel(com_read(bank, (uint32_t)spnt2 + 1));
                spnt2 += 2;

                for (int guard = 0; guard < 4096; guard++)
                {
                    if (sy >= 0 && sy < YMAX)
                    {
                        int x1 = round_to_pixel(sx);
                        int x2 = round_to_pixel(sx2);
                        if (x1 > x2)
                        {
                            int t = x1; x1 = x2; x2 = t;
                        }
                        if (x1 < 0) x1 = 0;
                        if (x2 >= XMAX) x2 = XMAX - 1;
                        if (x1 < x2)
                            memset(&bm[sy * 256 + x1 + 1], color, (size_t)(x2 - x1));
                    }
                    sy++;

                    if (sy > ey)
                    {
                        word1 = (int16_t)com_read(bank, (uint32_t)spnt);
                        ey = com_read(bank, (uint32_t)spnt + 1);
                        if (word1 == -1 && ey == 0xffff)
                            break;
                        ey = round_to_pixel(ey);
                        spnt += 2;
                    }
                    else
                        sx += word1;

                    if (sy > ey2)
                    {
                        word2 = (int16_t)com_read(bank, (uint32_t)spnt2);
                        ey2 = round_to_pixel(com_read(bank, (uint32_t)spnt2 + 1));
                        spnt2 += 2;
                    }
                    else
                        sx2 += word2;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Hooks called by the machine
// ---------------------------------------------------------------------------

void ref_math_start(uint16_t *ram, const uint8_t *rom_be, uint8_t *com)
{
    math_ram = ram;
    math_rom_be = rom_be;
    comram = com;
    irmb_run();
}

void ref_statwr(uint8_t data, uint8_t *com)
{
    comram = com;
    commbank = (data >> 7) & 1;
    bufsel = (data >> 1) & 1;
    if ((data & 0x01) && !vg_clear)
        memset(polybitmap[bufsel], 0, sizeof polybitmap[bufsel]);
    vg_clear = data & 0x01;
    if ((data & 0x04) && !(statwr_old & 0x04))
        run_video();
    statwr_old = data;
}

const uint8_t *ref_display_bitmap(void)
{
    return polybitmap[bufsel ^ 1];
}
