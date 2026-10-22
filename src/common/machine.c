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
#include "../../third_party/mc6809/mc6809.h"

void mc6809_reset(mc6809__t *cpu);
MC6809_RESULT mc6809_step(mc6809__t *cpu);

uint8_t ir_dip_3j = 0x00;
uint8_t ir_dip_5e = 0xFF;
bool ir_double_speed = false;

uint16_t ir_mathbox_mem[0x8000];
ir_dlist_t ir_dlist;

static mc6809__t cpu;
static ir_stats_t stats;
static ir_inputs_t in;

// Memory
static uint8_t ram_0000[0x800];
static uint8_t ram_0800[3][0x800];
static uint8_t comram[2][0x2000];
static uint8_t mathrom_be[6][0x2000];   // math box ROM as the 6809 sees it
static uint8_t prog_banked[6][0x2000];
static uint8_t prog_fixed[0xA000];
static uint8_t alpha_ram[0x400];
static uint8_t eeprom[0x100];
static uint8_t charset[64 * 8];
static uint32_t alpha_palette[2][4];
static uint32_t palette[64];

// Registers
static uint8_t statwr;      // $1140
static uint8_t out0;        // $1180
static uint8_t out1;        // $11C0
static uint8_t adc_result;
static int scanline;
static uint32_t lfsr = 0x1234567;

// Two bitmaps; BUFSEL picks which is drawn into and which is shown.
static uint8_t bitmap[2][IR_SCREEN_W * IR_SCREEN_H];

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
    ir_raster_list(bitmap[draw_target()], ir_dlist.words, ir_dlist.used, &stats);
    ir_dlist.used = 0;
}

void ir_dlist_add(int type, int color, const int32_t *xy, int count)
{
    if (count > IR_MAX_POINTS)
        count = IR_MAX_POINTS;
    if (ir_dlist.used + 1 + (uint32_t)count * 2 > IR_DLIST_WORDS)
        ir_dlist_flush();
    ir_dlist.words[ir_dlist.used++] = (int32_t)((type << 12) | (count << 6) | (color & 0x3F));
    for (int n = 0; n < count * 2; n++)
        ir_dlist.words[ir_dlist.used++] = xy[n];
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
        cpu.ReadPointer[p] = rd ? rd + off : NULL;
        cpu.WritePointer[p] = wr ? wr + off : NULL;
        cpu.ReadFunction[p] = rf;
        cpu.WriteFunction[p] = wf;
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
    uint16_t w = ir_mathbox_mem[(addr & 0x1FFF) >> 1];
    return (addr & 1) ? (uint8_t)w : (uint8_t)(w >> 8);
}

static void write_mathram(uint16 addr, uint8 data)
{
    uint16_t *w = &ir_mathbox_mem[(addr & 0x1FFF) >> 1];
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
        map(0x20, 0x3F, mathrom_be[rom_page[(out0 >> 1) & 7]], NULL, NULL, ignore_write);
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

static void bank_4000(void)
{
    int bank = (out1 >> 1) & 7;
    if (bank < 6)
        map(0x40, 0x5F, prog_banked[bank], NULL, NULL, ignore_write);
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

static void write_statwr(uint8_t data)
{
    uint8_t changed = statwr ^ data;
    statwr = data;

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
        stats.buffer_flips++;
    }

    // ERASE rising edge clears the (new) draw target, queued in order.
    if ((changed & STATWR_ERASE) && (data & STATWR_ERASE))
    {
        if (ir_dlist.used + 1 > IR_DLIST_WORDS)
            ir_dlist_flush();
        ir_dlist.words[ir_dlist.used++] = IR_CMD_ERASE << 12;
    }

    if ((changed & STATWR_MATH_START) && (data & STATWR_MATH_START))
    {
        ir_mathbox_start(&stats);
        cpu.FIRQ = true;
    }

    // EXT_START (active low) asks the video processor to draw the list.
    if ((changed & STATWR_EXT_START_N) && !(data & STATWR_EXT_START_N))
        ir_dlist_flush();
}

static void write_11xx(uint16 addr, uint8 data)
{
    if (addr <= 0x113F)
        cpu.IRQ = false;
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
        out1 = data;
        bank_4000();
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
    return (uint8_t)(lfsr ^ (uint32_t)cpu.Clock);
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
    palette[(addr >> 1) & 0x3F] = (uint32_t)(r << 16 | g << 8 | b);
}

static void write_1axx(uint16 addr, uint8 data)
{
    (void)addr; (void)data;
    cpu.FIRQ = false;
}

static void write_1bxx(uint16 addr, uint8 data)
{
    (void)data;
    adc_result = (addr & 1) ? in.joy_x : in.joy_y;
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

// The 2020 emulator replaces the expected math box self-test results in
// program bank 4 with what the high-level math box produces, then fixes the
// bank checksum. Without this the self test reports a math box failure.
static const uint8_t self_test_patch[72] = {
    0x40,0x00, 0x00,0x00, 0x01,0x21,  0x00,0x00, 0x40,0x00, 0x00,0x00,
    0xFE,0xE1, 0x00,0x00, 0x40,0x01,  0x40,0x01, 0xFE,0xE1, 0x01,0x21,
    0x01,0x21, 0x40,0x00, 0x00,0x00,  0xFE,0xE9, 0x00,0x05, 0x40,0x01,
    0x40,0x01, 0xFE,0xE8, 0x01,0x2C,  0x01,0x21, 0x40,0x01, 0xFE,0xE1,
    0xFE,0xE9, 0x01,0x22, 0x3F,0xFC,  0x40,0x01, 0x01,0x21, 0xFE,0xE9,
    0xFE,0xE8, 0x40,0x01, 0x01,0x22,  0x01,0x2C, 0xFE,0xE1, 0x3F,0xFC,
};

static void patch_self_test(void)
{
    memcpy(&prog_banked[4][0x5CAD - 0x4000], self_test_patch, sizeof self_test_patch);
    uint16_t sum = 0;
    for (int n = 0; n < 0x1FFE; n += 2)
        sum = (uint16_t)(sum - (prog_banked[4][n] * 256 + prog_banked[4][n + 1]));
    for (int n = 0; n < 0x2000; n += 2)
        sum = (uint16_t)(sum - (prog_banked[5][n] * 256 + prog_banked[5][n + 1]));
    prog_banked[4][0x1FFE] = (uint8_t)(sum >> 8);
    prog_banked[4][0x1FFF] = (uint8_t)sum;
}

static void load_roms(const ir_roms_t *r)
{
    memcpy(prog_banked[0], r->r405, 0x2000);
    memcpy(prog_banked[1], r->r405 + 0x2000, 0x2000);
    memcpy(prog_banked[2], r->r206, 0x2000);
    memcpy(prog_banked[3], r->r206 + 0x2000, 0x2000);
    memcpy(prog_banked[4], r->r207, 0x2000);
    memcpy(prog_banked[5], r->r207 + 0x2000, 0x2000);
    memcpy(prog_fixed + 0x0000, r->r208, 0x2000);
    memcpy(prog_fixed + 0x2000, r->r209, 0x4000);
    memcpy(prog_fixed + 0x6000, r->r210, 0x4000);
    patch_self_test();

    // Math box ROM: 103 (low) / 104 (high) at word 0x2000, 101 / 102 at 0x4000.
    memset(ir_mathbox_mem, 0, sizeof ir_mathbox_mem);
    for (int n = 0; n < 0x2000; n++)
        ir_mathbox_mem[0x2000 + n] = (uint16_t)(r->r104[n] << 8 | r->r103[n]);
    for (int n = 0; n < 0x4000; n++)
        ir_mathbox_mem[0x4000 + n] = (uint16_t)(r->r102[n] << 8 | r->r101[n]);
    for (int bank = 0; bank < 6; bank++)
        for (int n = 0; n < 0x1000; n++)
        {
            uint16_t w = ir_mathbox_mem[0x2000 + bank * 0x1000 + n];
            mathrom_be[bank][n * 2] = (uint8_t)(w >> 8);
            mathrom_be[bank][n * 2 + 1] = (uint8_t)w;
        }

    // Characters: two nibbles per row, high nibble first.
    for (int n = 0; n < 64 * 8; n++)
        charset[n] = (uint8_t)((r->r124[n * 2] << 4) | (r->r124[n * 2 + 1] & 0x0F));

    for (int p = 0; p < 2; p++)
        for (int c = 0; c < 4; c++)
        {
            uint8_t v = r->r125[p * 16 + c + 4];
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

void ir_init(const ir_roms_t *roms)
{
    memset(&stats, 0, sizeof stats);
    memset(&cpu, 0, sizeof cpu);
    memset(ram_0000, 0, sizeof ram_0000);
    memset(ram_0800, 0, sizeof ram_0800);
    memset(comram, 0, sizeof comram);
    memset(alpha_ram, 0, sizeof alpha_ram);
    memset(bitmap, 0, sizeof bitmap);
    ir_dlist.used = 0;
    load_roms(roms);

    map(0x00, 0xFF, NULL, NULL, read_unmapped, write_unmapped);
    map(0x00, 0x07, ram_0000, ram_0000, NULL, NULL);
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
    map(0x60, 0xFF, prog_fixed, NULL, NULL, ignore_write);

    statwr = 0x00;
    out0 = 0x10;
    out1 = 0x00;
    adc_result = 0x80;
    scanline = 0;
    bank_0800();
    bank_2000();
    bank_4000();

    mc6809_reset(&cpu);
}

// ---------------------------------------------------------------------------
// Frame loop
// ---------------------------------------------------------------------------

bool ir_run_frame(const ir_inputs_t *inputs)
{
    in = *inputs;

    // CPU cycles per 16-scanline slice, as a 16.16 fraction.
    const uint64_t clock = ir_double_speed ? 2ull * IR_CPU_HZ : IR_CPU_HZ;
    const uint32_t slice = (uint32_t)((clock * IR_DOTS_PER_LINE * 16 << 16) / IR_DOT_CLOCK_HZ);
    static uint32_t frac;
    static uint64_t target;

    for (int s = 0; s < IR_SCANLINES / 16; s++)
    {
        frac += slice;
        target += frac >> 16;
        frac &= 0xFFFF;

        while ((int64_t)(cpu.Clock - target) < 0)
        {
            if (mc6809_step(&cpu) != MC6809_RESULT_OK)
            {
                stats.cpu_faults++;
                return false;
            }
        }

        // IRQ is raised on scanlines 48, 112, 176, 240 and dropped by the
        // hardware 32 lines later if not acknowledged.
        scanline = (scanline + 16) & 0xFF;
        if (scanline & 0x10)
            cpu.IRQ = ((scanline - 16) & 0x20) != 0;
    }

    stats.frames++;
    return true;
}

// ---------------------------------------------------------------------------
// Video output
// ---------------------------------------------------------------------------

const uint8_t *ir_display_bitmap(void) { return bitmap[shown_buffer()]; }
const uint32_t *ir_bitmap_palette(void) { return palette; }
const uint8_t *ir_alpha_ram(void) { return alpha_ram; }
const uint8_t *ir_alpha_charset(void) { return charset; }
const uint32_t *ir_alpha_palette(void) { return alpha_palette[(out0 & 0x80) ? 1 : 0]; }
const ir_stats_t *ir_stats(void) { return &stats; }

void ir_compose_rgb(uint32_t *out)
{
    const uint8_t *bm = ir_display_bitmap();
    for (int n = 0; n < IR_SCREEN_W * IR_SCREEN_H; n++)
        out[n] = palette[bm[n] & 0x3F];

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
