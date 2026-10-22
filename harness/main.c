// PC test harness: runs the I, Robot machine model headless and writes
// screenshots, so the port can be checked without 32X hardware.
//
// Copyright 2026 the irobot 32X port authors. GPL-3.0-or-later, see LICENSE.
//
// Usage: irobot-harness [options]
//   --roms DIR        directory with the 136029-*.bin files (default roms/irobot)
//   --frames N        frames to run (default 1200, about 20 seconds)
//   --snap LIST       comma-separated frame numbers to save as PPM
//   --snap-every N    also save every Nth frame
//   --out DIR         where screenshots go (default build/snaps)
//   --press LIST      inputs as frame:button[:frames], e.g. 600:coin,700:start1
//                     buttons: coin, coinr, coina, start1, start2, fire, test, left, right, up, down
//   --dip3j N --dip5e N  DIP switch banks (default 0x00 and 0xFF)
//   --double-speed    run the 6809 at twice its real clock
//   --hash            print a hash of the displayed frame every frame
//   --text LIST       print the alphanumeric overlay as text on these frames
//   --recomp          run the recompiled 6809 code where it exists
//   --reference       run the math box microcode and draw like the video
//                     processor (harness/reference.c) instead of the
//                     high-level math box, for comparison
//   --wav FILE        record the sound to FILE (44.1 kHz, 16-bit mono)
//   --hints FILE      append (bank, address) of code the recompiler missed to
//                     FILE, for tools/recomp/recomp.py --hints

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/common/irobot.h"
#include "../src/common/pokey.h"
#include "reference.h"

extern void (*ir_ref_math_start)(uint16_t *math_ram, const uint8_t *math_rom_be, uint8_t *comram);
extern void (*ir_ref_statwr)(uint8_t data, uint8_t *comram);
extern const uint8_t *(*ir_display_override)(void);

extern FILE *ir_hints_file;    // machine.c, built with IR_HINTS

#define MAX_PRESSES 256

// Sound recording: the frame's POKEY writes are replayed at their times
// within the frame, and the output goes through a DC-blocking filter like
// the coupling capacitors of the real amplifier.
#define WAV_RATE 44100

static FILE *wav;
static uint32_t wav_samples;

static void wav_header(void)
{
    uint32_t bytes = wav_samples * 2;
    uint8_t h[44] = "RIFF....WAVEfmt ";
    uint32_t v[] = { 16, 1 | (1 << 16), WAV_RATE, WAV_RATE * 2, 2 | (16 << 16) };
    memcpy(h + 16, v, sizeof v);
    memcpy(h + 36, "data", 4);
    uint32_t riff = 36 + bytes;
    memcpy(h + 4, &riff, 4);
    memcpy(h + 40, &bytes, 4);
    fseek(wav, 0, SEEK_SET);
    fwrite(h, 1, 44, wav);
    fseek(wav, 0, SEEK_END);
}

static void wav_frame(void)
{
    static uint64_t due;    // samples owed, 32.32
    static int32_t x1, y1;  // filter state, 16.16
    static int16_t buf[4096];

    uint32_t cycles = ir_frame_cycles();
    due += ((uint64_t)WAV_RATE * cycles << 32) / IR_CPU_HZ;
    int n = (int)(due >> 32);
    due &= 0xFFFFFFFFu;
    if (n > 4096)
        n = 4096;

    int count, at = 0;
    const ir_sound_write_t *w = ir_sound_writes(&count);
    for (int i = 0; i <= count; i++)
    {
        int pos = i < count ? (int)((uint64_t)w[i].time * n / cycles) : n;
        if (pos > n)
            pos = n;
        if (pos > at)
        {
            ir_pokey_render(buf + at, pos - at);
            at = pos;
        }
        if (i < count)
            ir_pokey_write(w[i].reg, w[i].data);
    }

    for (int i = 0; i < n; i++)
    {
        int32_t x = buf[i] << 16;
        int32_t y = x - x1 + y1 - (y1 >> 9);    // about 14 Hz
        x1 = x;
        y1 = y;
        int32_t s = (y >> 16) * 400;
        int16_t out = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
        fwrite(&out, 2, 1, wav);
    }
    wav_samples += (uint32_t)n;
}

typedef struct { int frame, length; char button[16]; } press_t;

static press_t presses[MAX_PRESSES];
static int num_presses;

static uint8_t *load(const char *dir, const char *name, size_t size)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/136029-%s.bin", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        fprintf(stderr, "missing ROM %s\n", path);
        exit(1);
    }
    uint8_t *buf = malloc(size);
    size_t got = fread(buf, 1, size, f);
    fclose(f);
    if (got != size)
    {
        fprintf(stderr, "ROM %s is %zu bytes, expected %zu\n", path, got, size);
        exit(1);
    }
    return buf;
}

static void write_ppm(const char *path, const uint32_t *rgb)
{
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        perror(path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", IR_SCREEN_W, IR_SCREEN_H);
    for (int n = 0; n < IR_SCREEN_W * IR_SCREEN_H; n++)
    {
        uint8_t px[3] = { (uint8_t)(rgb[n] >> 16), (uint8_t)(rgb[n] >> 8), (uint8_t)rgb[n] };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

static void print_text(int frame)
{
    // Character codes 0-63 as ASCII approximations (from the 2020 emulator's debug charset).
    static const char map[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ-{}.:####v3 2 ?      0123456789chqt  ";
    const uint8_t *ram = ir_alpha_ram();
    printf("--- text at frame %d ---\n", frame);
    for (int row = 0; row < IR_SCREEN_H / 8; row++)
    {
        char line[33];
        for (int col = 0; col < 32; col++)
            line[col] = map[ram[row * 32 + col] & 0x3F];
        line[32] = 0;
        printf("%s\n", line);
    }
}

static uint32_t fnv1a(const uint32_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
    {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static void parse_presses(char *list)
{
    for (char *tok = strtok(list, ","); tok && num_presses < MAX_PRESSES; tok = strtok(NULL, ","))
    {
        press_t *p = &presses[num_presses];
        p->length = 10;
        if (sscanf(tok, "%d:%15[^:]:%d", &p->frame, p->button, &p->length) >= 2)
            num_presses++;
        else
            fprintf(stderr, "ignoring bad --press entry '%s'\n", tok);
    }
}

static void inputs_for_frame(int frame, ir_inputs_t *in)
{
    memset(in, 0, sizeof *in);
    in->joy_x = in->joy_y = 0x80;
    for (int i = 0; i < num_presses; i++)
    {
        const press_t *p = &presses[i];
        if (frame < p->frame || frame >= p->frame + p->length)
            continue;
        if (!strcmp(p->button, "coin"))        in->coin_left = true;
        else if (!strcmp(p->button, "coinr"))  in->coin_right = true;
        else if (!strcmp(p->button, "coina"))  in->coin_aux = true;
        else if (!strcmp(p->button, "start1")) in->start1 = true;
        else if (!strcmp(p->button, "start2")) in->start2 = true;
        else if (!strcmp(p->button, "fire"))   in->fire = true;
        else if (!strcmp(p->button, "test"))   in->test = true;
        else if (!strcmp(p->button, "left"))   in->joy_x = 0xFF;
        else if (!strcmp(p->button, "right"))  in->joy_x = 0x00;
        else if (!strcmp(p->button, "up"))     in->joy_y = 0x00;
        else if (!strcmp(p->button, "down"))   in->joy_y = 0xFF;
    }
}

int main(int argc, char **argv)
{
    const char *rom_dir = "roms/irobot";
    bool reference = false;
    const char *out_dir = "build/snaps";
    int frames = 1200, snap_every = 0;
    bool hash = false;
    char *snap_list = NULL, *text_list = NULL;

    for (int i = 1; i < argc; i++)
    {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (!strcmp(a, "--roms") && v)            { rom_dir = v; i++; }
        else if (!strcmp(a, "--frames") && v)     { frames = atoi(v); i++; }
        else if (!strcmp(a, "--snap") && v)       { snap_list = strdup(v); i++; }
        else if (!strcmp(a, "--snap-every") && v) { snap_every = atoi(v); i++; }
        else if (!strcmp(a, "--out") && v)        { out_dir = v; i++; }
        else if (!strcmp(a, "--press") && v)      { char *s = strdup(v); parse_presses(s); i++; }
        else if (!strcmp(a, "--dip3j") && v)      { ir_dip_3j = (uint8_t)strtol(v, NULL, 0); i++; }
        else if (!strcmp(a, "--dip5e") && v)      { ir_dip_5e = (uint8_t)strtol(v, NULL, 0); i++; }
        else if (!strcmp(a, "--double-speed"))    ir_double_speed = true;
        else if (!strcmp(a, "--hash"))            hash = true;
        else if (!strcmp(a, "--recomp"))          ir_use_recompiled = true;
        else if (!strcmp(a, "--reference"))       reference = true;
        else if (!strcmp(a, "--wav") && v)        { wav = fopen(v, "wb"); i++; }
        else if (!strcmp(a, "--hints") && v)      { ir_hints_file = fopen(v, "a"); i++; }
        else if (!strcmp(a, "--text") && v)       { text_list = strdup(v); i++; }
        else
        {
            fprintf(stderr, "unknown option %s\n", a);
            return 2;
        }
    }

    static int snaps[1024];
    int num_snaps = 0;
    if (snap_list)
        for (char *tok = strtok(snap_list, ","); tok && num_snaps < 1024; tok = strtok(NULL, ","))
            snaps[num_snaps++] = atoi(tok);

    static int texts[1024];
    int num_texts = 0;
    if (text_list)
        for (char *tok = strtok(text_list, ","); tok && num_texts < 1024; tok = strtok(NULL, ","))
            texts[num_texts++] = atoi(tok);

    ir_roms_t roms = {
        .r101 = load(rom_dir, "101", 0x4000), .r102 = load(rom_dir, "102", 0x4000),
        .r103 = load(rom_dir, "103", 0x2000), .r104 = load(rom_dir, "104", 0x2000),
        .r124 = load(rom_dir, "124", 0x800),  .r125 = load(rom_dir, "125", 0x20),
        .r206 = load(rom_dir, "206", 0x4000), .r207 = load(rom_dir, "207", 0x4000),
        .r405 = load(rom_dir, "405", 0x4000), .r208 = load(rom_dir, "208", 0x2000),
        .r209 = load(rom_dir, "209", 0x4000), .r210 = load(rom_dir, "210", 0x4000),
    };
    static ir_image_t image;
    ir_prepare_image(&roms, &image);
    if (reference)
    {
        static uint8_t proms[13 * 0x400];
        for (int n = 0; n < 13; n++)
        {
            char name[8];
            snprintf(name, sizeof name, "%d", 111 + n);
            uint8_t *p = load(rom_dir, name, 0x400);
            memcpy(&proms[n * 0x400], p, 0x400);
            free(p);
        }
        ref_init(proms);
        ir_ref_math_start = ref_math_start;
        ir_ref_statwr = ref_statwr;
        ir_display_override = ref_display_bitmap;
    }
    ir_init(&image);
    if (wav)
    {
        ir_pokey_init(WAV_RATE);
        wav_header();
    }

    static uint32_t rgb[IR_SCREEN_W * IR_SCREEN_H];
    ir_inputs_t in;
    int status = 0;

    for (int frame = 1; frame <= frames; frame++)
    {
        inputs_for_frame(frame, &in);
        if (!ir_run_frame(&in))
        {
            fprintf(stderr, "CPU fault in frame %d\n", frame);
            status = 1;
            break;
        }
        if (wav)
            wav_frame();

        bool snap = snap_every && frame % snap_every == 0;
        for (int i = 0; i < num_snaps && !snap; i++)
            snap = (snaps[i] == frame);

        for (int i = 0; i < num_texts; i++)
            if (texts[i] == frame)
                print_text(frame);

        if (snap || hash)
            ir_compose_rgb(rgb);
        if (hash)
            printf("frame %d %08x\n", frame, fnv1a(rgb, IR_SCREEN_W * IR_SCREEN_H));
        if (snap)
        {
            char path[1024];
            snprintf(path, sizeof path, "%s/frame%05d.ppm", out_dir, frame);
            write_ppm(path, rgb);
        }
    }

    const ir_stats_t *s = ir_stats();
    printf("frames %u, math box commands %u, objects %u, terrain passes %u\n",
           s->frames, s->mathbox_commands, s->objects, s->terrains);
    printf("polygons %u, vectors %u, dots %u, buffer flips %u\n",
           s->polygons, s->vectors, s->dots, s->buffer_flips);
    printf("unmapped reads %u, unmapped writes %u, CPU faults %u, idle cycles skipped %u\n",
           s->unmapped_reads, s->unmapped_writes, s->cpu_faults, s->idle_cycles);
    if (ir_use_recompiled)
        printf("interpreted instructions %u\n", s->interpreted);
    if (ir_hints_file)
        fclose(ir_hints_file);
    if (wav)
    {
        wav_header();
        fclose(wav);
    }
    return status;
}
