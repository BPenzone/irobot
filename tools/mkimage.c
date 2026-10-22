// Builds the prepared ROM image (ir_image_t) that the 32X build links into
// cartridge ROM.
//
// Copyright 2026 the irobot 32X port authors. GPL-3.0-or-later, see LICENSE.
//
// Usage: mkimage ROM_DIR OUT_FILE

#include <stdio.h>
#include <stdlib.h>

#include "../src/common/irobot.h"

static const uint8_t *load(const char *dir, const char *name, size_t size)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/136029-%s.bin", dir, name);
    FILE *f = fopen(path, "rb");
    uint8_t *buf = malloc(size);
    if (!f || fread(buf, 1, size, f) != size)
    {
        fprintf(stderr, "cannot read %s (%zu bytes expected); run make roms first\n", path, size);
        exit(1);
    }
    fclose(f);
    return buf;
}

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        fprintf(stderr, "usage: %s ROM_DIR OUT_FILE\n", argv[0]);
        return 2;
    }
    const char *dir = argv[1];
    ir_roms_t roms = {
        .r101 = load(dir, "101", 0x4000), .r102 = load(dir, "102", 0x4000),
        .r103 = load(dir, "103", 0x2000), .r104 = load(dir, "104", 0x2000),
        .r124 = load(dir, "124", 0x800),  .r125 = load(dir, "125", 0x20),
        .r206 = load(dir, "206", 0x4000), .r207 = load(dir, "207", 0x4000),
        .r405 = load(dir, "405", 0x4000), .r208 = load(dir, "208", 0x2000),
        .r209 = load(dir, "209", 0x4000), .r210 = load(dir, "210", 0x4000),
    };
    static ir_image_t image;
    ir_prepare_image(&roms, &image);

    FILE *f = fopen(argv[2], "wb");
    if (!f || fwrite(&image, sizeof image, 1, f) != 1 || fclose(f))
    {
        perror(argv[2]);
        return 1;
    }
    return 0;
}
