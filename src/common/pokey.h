// The four POKEY sound chips, shared by the PC harness and the 32X build.
//
// Copyright 2026 the irobot 32X port authors.
// Adapted from the Quad POKEY in John Manfreda's I, Robot Project emulator
// (https://github.com/manfreda-dot-org/I-Robot-Project), GPL-3.0-or-later,
// which is based on Ron Fries' POKEY Chip Emulator 2.3. See LICENSE.

#ifndef IR_POKEY_H
#define IR_POKEY_H

#include <stdint.h>

// POKEY clock on the I, Robot board: 12.096 MHz / 8.
#define IR_POKEY_HZ     1512000

// Loudest possible output: 16 channels at volume 15.
#define IR_POKEY_MAX    240

// Reset all four chips and set the output sample rate.
void ir_pokey_init(uint32_t sample_rate);

// A write to $1400-$143F, `reg` being the low 6 address bits.
void ir_pokey_write(uint8_t reg, uint8_t data);

// Render `n` samples, each 0..IR_POKEY_MAX, into `out`.
void ir_pokey_render(int16_t *out, int n);

#endif
