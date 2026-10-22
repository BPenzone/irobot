// I, Robot machine model shared by the PC harness and the 32X build.
//
// Copyright 2026 the irobot 32X port authors.
// Adapted from John Manfreda's I, Robot Project emulator
// (https://github.com/manfreda-dot-org/I-Robot-Project), GPL-3.0-or-later.
//
// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version. See LICENSE.

#ifndef IROBOT_H
#define IROBOT_H

#include <stdbool.h>
#include <stdint.h>

// Native picture size: 32 x 29 alphanumeric cells of 8 x 8 pixels.
#define IR_SCREEN_W 256
#define IR_SCREEN_H 232

// Video timing.
#define IR_SCANLINES        256
#define IR_VBLANK_SCANLINE  240
#define IR_CPU_HZ           1508625     // 12.069 MHz crystal / 8
#define IR_DOT_CLOCK_HZ     5000000
#define IR_DOTS_PER_LINE    320

// ROM images, already loaded into memory by the caller.
typedef struct
{
    const uint8_t *r101, *r102, *r103, *r104;   // math box data, 16K/16K/8K/8K
    const uint8_t *r124;                        // alphanumeric characters, 2K
    const uint8_t *r125;                        // alphanumeric color PROM, 32 bytes
    const uint8_t *r206, *r207, *r405;          // banked program, 16K each
    const uint8_t *r208, *r209, *r210;          // fixed program, 8K/16K/16K
} ir_roms_t;

// Player inputs; true means pressed.
typedef struct
{
    bool coin_left, coin_right, coin_aux;
    bool start1, start2, fire;
    bool test;
    uint8_t joy_x, joy_y;       // ADC values, 0x80 is centered
} ir_inputs_t;

// Counters useful for checking the harness against other emulators.
typedef struct
{
    uint32_t frames;
    uint32_t mathbox_commands;
    uint32_t objects, terrains;
    uint32_t polygons, vectors, dots;
    uint32_t buffer_flips;
    uint32_t unmapped_reads, unmapped_writes;
    uint32_t cpu_faults;
} ir_stats_t;

// Initialize the machine from a ROM set and perform a cold reset.
void ir_init(const ir_roms_t *roms);

// Run the 6809 for one video frame (256 scanlines). Returns false if the
// CPU faulted (illegal instruction or unmapped access it cannot survive).
bool ir_run_frame(const ir_inputs_t *inputs);

// The 8-bit bitmap currently being displayed, IR_SCREEN_W x IR_SCREEN_H,
// each byte a 6-bit index into the color RAM palette.
const uint8_t *ir_display_bitmap(void);

// 64-entry palette for the bitmap, as 0x00RRGGBB.
const uint32_t *ir_bitmap_palette(void);

// Alphanumeric overlay: 32 x 32 bytes of character RAM (29 rows visible),
// 64 characters of 8 rows each in the character ROM, and the 4-color
// palette currently selected (color 0 is transparent).
const uint8_t *ir_alpha_ram(void);
const uint8_t *ir_alpha_charset(void);  // 64 chars x 8 rows, MSB is leftmost
const uint32_t *ir_alpha_palette(void);

// Composite the bitmap and alphanumerics into 0x00RRGGBB pixels.
void ir_compose_rgb(uint32_t *out);

const ir_stats_t *ir_stats(void);

// DIP switches. Defaults match the 2020 emulator: 3J = 0x00, 5E = 0xFF.
extern uint8_t ir_dip_3j, ir_dip_5e;

// Run the 6809 at twice the real clock, as the 2020 emulator does by default.
extern bool ir_double_speed;

#endif
