// Rearranges a ROM set into the layout the machine uses (ir_image_t).
//
// Copyright 2026 the irobot 32X port authors.
// Adapted from John Manfreda's I, Robot Project emulator (2020 RomSet.cs and
// Machine.cs), https://github.com/manfreda-dot-org/I-Robot-Project.
//
// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version. See LICENSE.

#include <string.h>

#include "irobot.h"

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

static void patch_self_test(uint8_t (*banks)[0x2000])
{
    memcpy(&banks[4][0x5CAD - 0x4000], self_test_patch, sizeof self_test_patch);
    uint16_t sum = 0;
    for (int n = 0; n < 0x1FFE; n += 2)
        sum = (uint16_t)(sum - (banks[4][n] * 256 + banks[4][n + 1]));
    for (int n = 0; n < 0x2000; n += 2)
        sum = (uint16_t)(sum - (banks[5][n] * 256 + banks[5][n + 1]));
    banks[4][0x1FFE] = (uint8_t)(sum >> 8);
    banks[4][0x1FFF] = (uint8_t)sum;
}

void ir_prepare_image(const ir_roms_t *r, ir_image_t *img)
{
    memcpy(img->prog_banked[0], r->r405, 0x2000);
    memcpy(img->prog_banked[1], r->r405 + 0x2000, 0x2000);
    memcpy(img->prog_banked[2], r->r206, 0x2000);
    memcpy(img->prog_banked[3], r->r206 + 0x2000, 0x2000);
    memcpy(img->prog_banked[4], r->r207, 0x2000);
    memcpy(img->prog_banked[5], r->r207 + 0x2000, 0x2000);
    memcpy(img->prog_fixed + 0x0000, r->r208, 0x2000);
    memcpy(img->prog_fixed + 0x2000, r->r209, 0x4000);
    memcpy(img->prog_fixed + 0x6000, r->r210, 0x4000);
    patch_self_test(img->prog_banked);

    // Math box ROM: 103 (low) / 104 (high) at word 0x2000, 101 / 102 at 0x4000.
    uint8_t *m = &img->mathrom_be[0][0];
    for (int n = 0; n < 0x2000; n++)
    {
        m[n * 2] = r->r104[n];
        m[n * 2 + 1] = r->r103[n];
    }
    m += 0x4000;
    for (int n = 0; n < 0x4000; n++)
    {
        m[n * 2] = r->r102[n];
        m[n * 2 + 1] = r->r101[n];
    }

    // Characters: two nibbles per row, high nibble first.
    for (int n = 0; n < 64 * 8; n++)
        img->charset[n] = (uint8_t)((r->r124[n * 2] << 4) | (r->r124[n * 2 + 1] & 0x0F));

    memcpy(img->alpha_prom, r->r125, sizeof img->alpha_prom);
}
