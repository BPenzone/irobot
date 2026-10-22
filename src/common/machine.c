// I, Robot machine: memory map, I/O registers, banking, interrupts and the
// video buffers.
//
// Copyright 2026 the irobot 32X port authors.
// Adapted from John Manfreda's I, Robot Project emulator (2020 Machine.cs and
// Emulation/Subsystems/*.cs, 1998 IO.c and BankSwitch.c),
// https://github.com/manfreda-dot-org/I-Robot-Project.
//
// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version. See LICENSE.

#include <string.h>

#include "internal.h"
#include "rc_runtime.h"

void mc6809_reset(mc6809__t *cpu);
MC6809_RESULT mc6809_step(mc6809__t *cpu);

bool ir_use_recompiled = false;
uint8_t ir_dip_3j = 0x00;
uint8_t ir_dip_5e = 0xFF;
bool ir_double_speed = false;

uint16_t ir_math_ram[0x1000];
const uint16_t *ir_math_rom;

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define MATHROM_IS_HOST_ORDER 1
#else
static uint16_t math_rom_host[0x6000];
#endif

// Display list being filled by the math box.
static int32_t *dlist;
static uint32_t dlist_used;
#ifndef IR_DLIST_EXTERNAL
static int32_t dlist_buffer[IR_DLIST_WORDS];
#endif

mc6809__t ir_cpu;
static ir_stats_t stats;
static ir_inputs_t in;

// Memory
uint8_t ir_ram0[0x800];   // also used directly by recompiled code
static uint8_t ram_0800[3][0x800];
static uint8_t comram[2][0x2000];
static uint8_t alpha_ram[0x400];
static uint8_t eeprom[0x100];
static uint32_t alpha_palette[2][4];
static const ir_image_t *image;

// Registers
static uint8_t statwr;      // $1140
static uint8_t out0;        // $1180
static uint8_t out1;        // $11C0
static uint8_t adc_result;
static int scanline;
static uint32_t lfsr = 0x1234567;

// The video backend keeps two bitmaps; BUFSEL picks which is drawn into and
// which is shown.

#define STATWR_ERASE        0x01
#define STATWR_BUFSEL       0x02
#define STATWR_EXT_START_N  0x04    // active low
#define STATWR_MATH_START   0x10
#define STATWR_EXT_COM_SWAP 0x80

static inline int draw_target(void) { return (statwr & STATWR_BUFSEL) ? 0 : 1; }
static inline int shown_buffer(void) { return (statwr & STATWR_BUFSEL) ? 1 : 0; }

// ---------------------------------------------------------------------------
// Display list plumbing
// ---------------------------------------------------------------------------

void ir_dlist_flush(void)
{
    if (dlist_used)
        dlist = ir_video_submit(draw_target(), dlist, dlist_used, &stats);
    dlist_used = 0;
}

void ir_dlist_add(int type, int color, const int32_t *xy, int count)
{
    if (count > IR_MAX_POINTS)
        count = IR_MAX_POINTS;
    if (dlist_used + 1 + (uint32_t)count * 2 > IR_DLIST_WORDS)
        ir_dlist_flush();
    dlist[dlist_used++] = (int32_t)((type << 12) | (count << 6) | (color & 0x3F));
    for (int n = 0; n < count * 2; n++)
        dlist[dlist_used++] = xy[n];
}

// ---------------------------------------------------------------------------
// Page mapping helpers
// ---------------------------------------------------------------------------

static void map(uint8_t first, uint8_t last, uint8_t *rd, uint8_t *wr,
                pReadFunction rf, pWriteFunction wf)
{
    for (unsigned p = first; p <= last; p++)
    {
        unsigned off = (p - first) << 8;
        ir_cpu.ReadPointer[p] = rd ? rd + off : NULL;
        ir_cpu.WritePointer[p] = wr ? wr + off : NULL;
        ir_cpu.ReadFunction[p] = rf;
        ir_cpu.WriteFunction[p] = wf;
    }
}

static uint8 read_unmapped(uint16 addr)
{
    (void)addr;
    stats.unmapped_reads++;
    return 0xFF;
}

static void write_unmapped(uint16 addr, uint8 data)
{
    (void)addr; (void)data;
    stats.unmapped_writes++;
}

static void ignore_write(uint16 addr, uint8 data)
{
    (void)addr; (void)data;
}

// Math box RAM is 16-bit; the 6809 sees each word high byte first.
static uint8 read_mathram(uint16 addr)
{
    uint16_t w = ir_math_ram[(addr & 0x1FFF) >> 1];
    return (addr & 1) ? (uint8_t)w : (uint8_t)(w >> 8);
}

static void write_mathram(uint16 addr, uint8 data)
{
    uint16_t *w = &ir_math_ram[(addr & 0x1FFF) >> 1];
    if (addr & 1)
        *w = (uint16_t)((*w & 0xFF00) | data);
    else
        *w = (uint16_t)((*w & 0x00FF) | (data << 8));
}

// $2000-$3FFF: math box ROM page, math box RAM, or one of two COM RAMs.
static void bank_2000(void)
{
    if (!(out0 & 0x10))
    {
        static const uint8_t rom_page[8] = { 0, 1, 0, 1, 2, 3, 4, 5 };
        map(0x20, 0x3F, (uint8_t *)image->mathrom_be[rom_page[(out0 >> 1) & 7]], NULL, NULL, ignore_write);
    }
    else if (out0 & 0x08)
        map(0x20, 0x3F, NULL, NULL, read_mathram, write_mathram);
    else
    {
        uint8_t *ram = comram[(statwr & STATWR_EXT_COM_SWAP) ? 1 : 0];
        map(0x20, 0x3F, ram, ram, NULL, NULL);
    }
}

static void bank_0800(void)
{
    int bank = (out0 >> 5) & 3;
    if (bank < 3)
        map(0x08, 0x0F, ram_0800[bank], ram_0800[bank], NULL, NULL);
    else
        map(0x08, 0x0F, NULL, NULL, read_unmapped, write_unmapped);
}

uint8_t ir_prog_bank;
uint32_t rc_limit;

static void bank_4000(void)
{
    int bank = (out1 >> 1) & 7;
    ir_prog_bank = (uint8_t)bank;
    if (bank < 6)
        map(0x40, 0x5F, (uint8_t *)image->prog_banked[bank], NULL, NULL, ignore_write);
    else
        map(0x40, 0x5F, NULL, NULL, read_unmapped, write_unmapped);
}

// ---------------------------------------------------------------------------
// I/O
// ---------------------------------------------------------------------------

static uint8 read_10xx(uint16 addr)
{
    uint8_t v = 0xFF;
    if (addr <= 0x103F)
    {
        if (in.coin_right) v &= (uint8_t)~0x80;
        if (in.coin_left)  v &= (uint8_t)~0x40;
        if (in.coin_aux)   v &= (uint8_t)~0x20;
        if (in.test)       v &= (uint8_t)~0x10;
        return v;
    }
    if (addr <= 0x107F)
    {
        if (in.start1) v &= (uint8_t)~0x80;
        if (in.start2) v &= (uint8_t)~0x40;
        if (in.fire)   v &= (uint8_t)~0x10;
        return v;
    }
    if (addr <= 0x10BF)
    {
        // bit 7 VBLANK, bit 6 EXT_DONE (0 = done), bit 5 MB_DONE (1 = done)
        v = 0x20;
        if (scanline >= IR_VBLANK_SCANLINE)
            v |= 0x80;
        return v;
    }
    return ir_dip_3j;
}

// The PC harness can swap in a reference math box and video processor.
void (*ir_ref_math_start)(uint16_t *math_ram, const uint8_t *math_rom_be, uint8_t *comram);
void (*ir_ref_statwr)(uint8_t data, uint8_t *comram);

static void write_statwr(uint8_t data)
{
    uint8_t changed = statwr ^ data;
    statwr = data;

    if (ir_ref_statwr)
        ir_ref_statwr(data, &comram[0][0]);

    if (changed & STATWR_EXT_COM_SWAP)
        bank_2000();

    // BUFSEL toggles which bitmap is shown. Finish drawing into the old
    // target first; the queued commands belong to it. The game usually
    // flips and raises ERASE in the same write, so this must come first.
    if (changed & STATWR_BUFSEL)
    {
        statwr ^= STATWR_BUFSEL;
        ir_dlist_flush();
        statwr ^= STATWR_BUFSEL;
        ir_video_show(shown_buffer());
        stats.buffer_flips++;
    }

    // ERASE rising edge clears the (new) draw target, queued in order.
    if ((changed & STATWR_ERASE) && (data & STATWR_ERASE))
    {
        if (dlist_used + 1 > IR_DLIST_WORDS)
            ir_dlist_flush();
        dlist[dlist_used++] = IR_CMD_ERASE << 12;
    }

    if ((changed & STATWR_MATH_START) && (data & STATWR_MATH_START))
    {
        if (ir_ref_math_start)
            ir_ref_math_start(ir_math_ram, &image->mathrom_be[0][0], &comram[0][0]);
        else
            ir_mathbox_start(&stats);
        ir_cpu.FIRQ = true;
        rc_limit = ir_cpu.Clock;    // leave recompiled code to take it
    }

    // EXT_START (active low) asks the video processor to draw the list.
    if ((changed & STATWR_EXT_START_N) && !(data & STATWR_EXT_START_N))
        ir_dlist_flush();
}

static void write_11xx(uint16 addr, uint8 data)
{
    if (addr <= 0x113F)
        ir_cpu.IRQ = false;
    else if (addr <= 0x117F)
        write_statwr(data);
    else if (addr <= 0x11BF)
    {
        out0 = data;
        bank_0800();
        bank_2000();
    }
    else
    {
        bool same_bank = ((out1 ^ data) & 0x0E) == 0;
        out1 = data;
        if (same_bank)
            return;
        bank_4000();
        // Recompiled code chains blocks within the bank it started in, so
        // make it come back to the dispatcher after a bank switch.
        rc_limit = ir_cpu.Clock;
    }
}

static uint8 read_12xx(uint16 addr)
{
    return eeprom[addr & 0xFF] & 0x0F;
}

static void write_12xx(uint16 addr, uint8 data)
{
    eeprom[addr & 0xFF] = data & 0x0F;
}

static uint8 read_13xx(uint16 addr)
{
    (void)addr;
    return adc_result;
}

static uint8 read_14xx(uint16 addr)
{
    if ((addr & 0x3F) == 0x20)
        return ir_dip_5e;
    // POKEY RANDOM register. The real chip returns bits of a free-running
    // 17-bit polynomial counter, so successive reads are effectively
    // independent. A one-bit shift per read is not: the game's placement
    // loop at $76D4 can then retry long enough to trip its own watchdog.
    lfsr ^= lfsr << 13;
    lfsr ^= lfsr >> 17;
    lfsr ^= lfsr << 5;
    return (uint8_t)(lfsr ^ (uint32_t)ir_cpu.Clock);
}

static void write_18xx(uint16 addr, uint8 data)
{
    // Color RAM: data rrggbbii and address bit 0 are written inverted;
    // intensity is 3 bits, two from data and one from the address.
    data = (uint8_t)~data;
    int i = ((data & 0x03) << 1) + ((~addr) & 1);
    int r = 255 * ((data >> 6) & 3) * i / 21;
    int g = 255 * ((data >> 4) & 3) * i / 21;
    int b = 255 * ((data >> 2) & 3) * i / 21;
    ir_video_set_color((addr >> 1) & 0x3F, (uint8_t)r, (uint8_t)g, (uint8_t)b);
}

static void write_1axx(uint16 addr, uint8 data)
{
    (void)addr; (void)data;
    ir_cpu.FIRQ = false;
}

static void write_1bxx(uint16 addr, uint8 data)
{
    (void)data;
    adc_result = (addr & 1) ? in.joy_x : in.joy_y;
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

static void load_image(const ir_image_t *img)
{
    image = img;

#ifdef MATHROM_IS_HOST_ORDER
    ir_math_rom = (const uint16_t *)img->mathrom_be;
#else
    const uint8_t *m = &img->mathrom_be[0][0];
    for (int n = 0; n < 0x6000; n++)
        math_rom_host[n] = (uint16_t)(m[n * 2] << 8 | m[n * 2 + 1]);
    ir_math_rom = math_rom_host;
#endif

    for (int p = 0; p < 2; p++)
        for (int c = 0; c < 4; c++)
        {
            uint8_t v = img->alpha_prom[p * 16 + c + 4];
            int i = (v & 3) + 1;
            int red = 255 * ((v >> 6) & 3) * i / 12;
            int grn = 255 * ((v >> 4) & 3) * i / 12;
            int blu = 255 * ((v >> 2) & 3) * i / 12;
            if (red > 255) red = 255;
            if (grn > 255) grn = 255;
            if (blu > 255) blu = 255;
            alpha_palette[p][c] = (uint32_t)(red << 16 | grn << 8 | blu);
        }
}

void ir_init(const ir_image_t *img)
{
    memset(&stats, 0, sizeof stats);
    memset(&ir_cpu, 0, sizeof ir_cpu);
    memset(ir_ram0, 0, sizeof ir_ram0);
    memset(ram_0800, 0, sizeof ram_0800);
    memset(comram, 0, sizeof comram);
    memset(alpha_ram, 0, sizeof alpha_ram);
    memset(ir_math_ram, 0, sizeof ir_math_ram);
    load_image(img);
    ir_video_reset();
#ifdef IR_DLIST_EXTERNAL
    dlist = ir_video_submit(0, NULL, 0, &stats);
#else
    dlist = dlist_buffer;
#endif
    dlist_used = 0;

    map(0x00, 0xFF, NULL, NULL, read_unmapped, write_unmapped);
    map(0x00, 0x07, ir_ram0, ir_ram0, NULL, NULL);
    map(0x10, 0x10, NULL, NULL, read_10xx, write_unmapped);
    map(0x11, 0x11, NULL, NULL, read_unmapped, write_11xx);
    map(0x12, 0x12, NULL, NULL, read_12xx, write_12xx);
    map(0x13, 0x13, NULL, NULL, read_13xx, write_unmapped);
    map(0x14, 0x14, NULL, NULL, read_14xx, ignore_write);    // POKEYs
    map(0x18, 0x18, NULL, NULL, read_unmapped, write_18xx);
    map(0x19, 0x19, NULL, NULL, read_unmapped, ignore_write); // watchdog
    map(0x1A, 0x1A, NULL, NULL, read_unmapped, write_1axx);
    map(0x1B, 0x1B, NULL, NULL, read_unmapped, write_1bxx);
    map(0x1C, 0x1F, alpha_ram, alpha_ram, NULL, NULL);
    map(0x60, 0xFF, (uint8_t *)image->prog_fixed, NULL, NULL, ignore_write);

    statwr = 0x00;
    out0 = 0x10;
    out1 = 0x00;
    adc_result = 0x80;
    scanline = 0;
    bank_0800();
    bank_2000();
    bank_4000();

    mc6809_reset(&ir_cpu);
}

// ---------------------------------------------------------------------------
// Frame loop
// ---------------------------------------------------------------------------

#ifdef IR_HINTS
#include <stdio.h>
FILE *ir_hints_file;

// Remember each address once per bank so the hints file stays small.
static void record_hint(uint16_t pc)
{
    static uint8_t seen[7][0x10000 / 8];
    int ctx = pc >= 0x6000 ? 6 : ir_prog_bank;
    if (ctx > 6 || (seen[ctx][pc >> 3] & (1 << (pc & 7))))
        return;
    seen[ctx][pc >> 3] |= (uint8_t)(1 << (pc & 7));
    if (ctx == 6)
        fprintf(ir_hints_file, "F %04X\n", pc);
    else
        fprintf(ir_hints_file, "%d %04X\n", ctx, pc);
}
#endif

#define IDLE_LOOP_PC    0x8E5E
#define IDLE_FLAG       0x07F0

static inline bool interrupt_pending(void)
{
    return (ir_cpu.IRQ && !ir_cpu.CC_I) || (ir_cpu.FIRQ && !ir_cpu.CC_F) || ir_cpu.NMI;
}

bool ir_run_frame(const ir_inputs_t *inputs)
{
    in = *inputs;

    // CPU cycles per 16-scanline slice, as a 16.16 fraction.
    const uint64_t clock = ir_double_speed ? 2ull * IR_CPU_HZ : IR_CPU_HZ;
    const uint32_t slice = (uint32_t)((clock * IR_DOTS_PER_LINE * 16 << 16) / IR_DOT_CLOCK_HZ);
    static uint32_t frac;
    static uint32_t target;

    for (int s = 0; s < IR_SCANLINES / 16; s++)
    {
        frac += slice;
        target += frac >> 16;
        frac &= 0xFFFF;

        while ((int32_t)(ir_cpu.Clock - target) < 0)
        {
            // The main loop idles at $8E5E (LDA $07F0 / BEQ $8E5E) until the
            // IRQ handler sets $07F0. Nothing can change it before the next
            // interrupt, so skip to the end of the slice instead of spinning.
            if (ir_cpu.PC.w == IDLE_LOOP_PC && !ir_ram0[IDLE_FLAG] && !interrupt_pending())
            {
                stats.idle_cycles += target - ir_cpu.Clock;
                ir_cpu.Clock = target;
                break;
            }
            if (ir_use_recompiled && !interrupt_pending())
            {
                void *b = rc_dispatch(ir_cpu.PC.w);
                if (b)
                {
                    rc_limit = target;
                    while ((b = ((rc_block_t)b)()))
                        ;
                    continue;
                }
                stats.interpreted++;
#ifdef IR_HINTS
                if (ir_hints_file && ir_cpu.PC.w >= 0x4000)
                    record_hint(ir_cpu.PC.w);
#endif
            }
            if (mc6809_step(&ir_cpu) != MC6809_RESULT_OK)
            {
                stats.cpu_faults++;
                return false;
            }
        }

        // IRQ is raised on scanlines 48, 112, 176, 240 and dropped by the
        // hardware 32 lines later if not acknowledged.
        scanline = (scanline + 16) & 0xFF;
        if (scanline & 0x10)
            ir_cpu.IRQ = ((scanline - 16) & 0x20) != 0;
    }

    stats.frames++;
    return true;
}

// ---------------------------------------------------------------------------
// Video output
// ---------------------------------------------------------------------------

const uint8_t *ir_alpha_ram(void) { return alpha_ram; }
const uint8_t *ir_alpha_charset(void) { return image->charset; }
int ir_alpha_palette_select(void) { return (out0 & 0x80) ? 1 : 0; }
const uint32_t *ir_alpha_palette(void) { return alpha_palette[ir_alpha_palette_select()]; }
const ir_stats_t *ir_stats(void) { return &stats; }
