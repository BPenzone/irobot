// Reference math box and video processor for the PC harness, see reference.c.

#ifndef IROBOT_REFERENCE_H
#define IROBOT_REFERENCE_H

#include <stdint.h>

void ref_init(const uint8_t *proms);    // 136029-111 .. -123, 0x400 bytes each
void ref_math_start(uint16_t *math_ram, const uint8_t *math_rom_be, uint8_t *comram);
void ref_statwr(uint8_t data, uint8_t *comram);
const uint8_t *ref_display_bitmap(void);    // 256 x IR_SCREEN_H

#endif
