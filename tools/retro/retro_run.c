// Headless libretro frontend: loads a core (PicoDrive for 32X), runs a ROM
// for a number of frames with scripted pad input, and writes PPM screenshots.
//
// Copyright 2026 the irobot 32X port authors. GPL-3.0-or-later, see LICENSE.
//
// Usage: retro-run CORE.so ROM [--frames N] [--snap LIST] [--snap-every N]
//                  [--out DIR] [--press frame:button[:len],...] [--hash]
//   buttons: up down left right a b c x y z start mode
//   --picodrive-dump FILE  after the last frame, save the 32X SDRAM to FILE,
//                          both frame buffers to FILE.fb (and the SH-2 profile
//                          to FILE.prof, see tools/retro/prof32x.py), and
//                          print both SH-2 PCs (PicoDrive only)
//   The environment variable RETRO_DRC sets the core's picodrive_drc option.

#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libretro.h"

static unsigned pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;
static unsigned frame_w, frame_h;
static uint32_t *frame_rgb;
static bool pad[16];

static void log_cb(enum retro_log_level level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[core] ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

// Core options that differ from what the core assumes when the frontend has none.
static struct { const char *key, *value; } options[] = {
    { "picodrive_renderer", "accurate" },
    { "picodrive_input1", "6 button pad" },
    { "picodrive_drc", "enabled" },
};

static bool environment(unsigned cmd, void *data)
{
    switch (cmd)
    {
    case RETRO_ENVIRONMENT_GET_VARIABLE:
    {
        struct retro_variable *var = data;
        for (size_t i = 0; i < sizeof options / sizeof options[0]; i++)
            if (!strcmp(var->key, options[i].key))
            {
                var->value = options[i].value;
                return true;
            }
        var->value = NULL;
        return false;
    }
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        pixel_format = *(const unsigned *)data;
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = log_cb;
        return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = ".";
        return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;
    default:
        return false;
    }
}

static void video(const void *data, unsigned w, unsigned h, size_t pitch)
{
    if (!data)
        return;
    if (w * h > frame_w * frame_h)
        frame_rgb = realloc(frame_rgb, w * h * sizeof *frame_rgb);
    frame_w = w;
    frame_h = h;
    for (unsigned y = 0; y < h; y++)
    {
        const uint8_t *row = (const uint8_t *)data + y * pitch;
        for (unsigned x = 0; x < w; x++)
        {
            uint32_t c;
            if (pixel_format == RETRO_PIXEL_FORMAT_XRGB8888)
                c = ((const uint32_t *)row)[x] & 0xFFFFFF;
            else
            {
                uint16_t p = ((const uint16_t *)row)[x];
                unsigned r, g, b;
                if (pixel_format == RETRO_PIXEL_FORMAT_RGB565)
                    r = (p >> 11) << 3, g = ((p >> 5) & 0x3F) << 2, b = (p & 0x1F) << 3;
                else
                    r = ((p >> 10) & 0x1F) << 3, g = ((p >> 5) & 0x1F) << 3, b = (p & 0x1F) << 3;
                c = r << 16 | g << 8 | b;
            }
            frame_rgb[y * w + x] = c;
        }
    }
}

static void audio(int16_t l, int16_t r) {}
static size_t audio_batch(const int16_t *d, size_t frames) { return frames; }
static void input_poll(void) {}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    if (port != 0 || device != RETRO_DEVICE_JOYPAD)
        return 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
    {
        int16_t m = 0;
        for (int i = 0; i < 16; i++)
            if (pad[i])
                m |= 1 << i;
        return m;
    }
    return id < 16 && pad[id];
}

typedef struct { int frame, length, id; } press_t;
static press_t presses[256];
static int num_presses;

static int button_id(const char *s)
{
    static const struct { const char *name; int id; } map[] = {
        { "up", RETRO_DEVICE_ID_JOYPAD_UP }, { "down", RETRO_DEVICE_ID_JOYPAD_DOWN },
        { "left", RETRO_DEVICE_ID_JOYPAD_LEFT }, { "right", RETRO_DEVICE_ID_JOYPAD_RIGHT },
        // PicoDrive maps B->A, A->B, Y->C on a 3-button pad
        { "a", RETRO_DEVICE_ID_JOYPAD_Y }, { "b", RETRO_DEVICE_ID_JOYPAD_B },
        { "c", RETRO_DEVICE_ID_JOYPAD_A }, { "x", RETRO_DEVICE_ID_JOYPAD_L },
        { "y", RETRO_DEVICE_ID_JOYPAD_X }, { "z", RETRO_DEVICE_ID_JOYPAD_R },
        { "start", RETRO_DEVICE_ID_JOYPAD_START }, { "mode", RETRO_DEVICE_ID_JOYPAD_SELECT },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strcmp(s, map[i].name))
            return map[i].id;
    return -1;
}

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        perror(path);
        return;
    }
    fprintf(f, "P6\n%u %u\n255\n", frame_w, frame_h);
    for (unsigned n = 0; n < frame_w * frame_h; n++)
    {
        uint8_t px[3] = { (uint8_t)(frame_rgb[n] >> 16), (uint8_t)(frame_rgb[n] >> 8), (uint8_t)frame_rgb[n] };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

// PicoDrive internals: Pico32xMem->sdram is the first member, stored with
// the bytes of each 16-bit word swapped; sh2s[2] are the SH-2 states.
#define PD_SH2_SIZE 7232
#define PD_SH2_PC   0x40
#define PD_SH2_PR   0x48

static void picodrive_dump(void *core, const char *path)
{
    uint8_t **mem = dlsym(core, "Pico32xMem");
    uint8_t *sh2s = dlsym(core, "sh2s");
    if (!mem || !*mem || !sh2s)
    {
        fprintf(stderr, "core has no 32X state to dump\n");
        return;
    }
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        perror(path);
        return;
    }
    for (int n = 0; n < 0x40000; n += 2)
    {
        uint8_t w[2] = { (*mem)[n + 1], (*mem)[n] };
        fwrite(w, 1, 2, f);
    }
    fclose(f);
    // Both frame buffers follow the SDRAM and the DRC block maps.
    char fb_path[1024];
    snprintf(fb_path, sizeof fb_path, "%s.fb", path);
    if ((f = fopen(fb_path, "wb")))
    {
        for (int n = 0x80000; n < 0xC0000; n += 2)
        {
            uint8_t w[2] = { (*mem)[n + 1], (*mem)[n] };
            fwrite(w, 1, 2, f);
        }
        fclose(f);
    }
    // A core built with the SH-2 profiling change (see tools/retro/prof32x.py)
    // keeps cycles per instruction address in sh2_prof; save it beside the dump.
    const uint32_t *prof = dlsym(core, "sh2_prof");
    char prof_path[1024];
    snprintf(prof_path, sizeof prof_path, "%s.prof", path);
    if (prof && (f = fopen(prof_path, "wb")))
    {
        fwrite(prof, 4, 2 * 0x240000, f);
        fclose(f);
    }
    for (int i = 0; i < 2; i++)
    {
        const uint8_t *s = sh2s + i * PD_SH2_SIZE;
        printf("%s SH-2 pc %08x pr %08x\n", i ? "slave " : "master",
               *(const uint32_t *)(s + PD_SH2_PC), *(const uint32_t *)(s + PD_SH2_PR));
    }
}

#define SYM(name) do { *(void **)&p_##name = dlsym(core, #name); if (!p_##name) { fprintf(stderr, "core lacks %s\n", #name); return 1; } } while (0)

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s CORE.so ROM [options]\n", argv[0]);
        return 2;
    }
    const char *out_dir = "build/snaps32x";
    int frames = 300, snap_every = 0, num_snaps = 0;
    static int snaps[1024];
    bool hash = false;
    const char *dump_path = NULL;

    if (getenv("RETRO_DRC"))
        options[2].value = getenv("RETRO_DRC");
    for (int i = 3; i < argc; i++)
    {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--frames") && v)          { frames = atoi(v); i++; }
        else if (!strcmp(a, "--snap-every") && v) { snap_every = atoi(v); i++; }
        else if (!strcmp(a, "--out") && v)        { out_dir = v; i++; }
        else if (!strcmp(a, "--hash"))            hash = true;
        else if (!strcmp(a, "--picodrive-dump") && v) { dump_path = v; i++; }
        else if (!strcmp(a, "--snap") && v)
        {
            char *s = strdup(v);
            for (char *t = strtok(s, ","); t && num_snaps < 1024; t = strtok(NULL, ","))
                snaps[num_snaps++] = atoi(t);
            i++;
        }
        else if (!strcmp(a, "--press") && v)
        {
            char *s = strdup(v);
            for (char *t = strtok(s, ","); t && num_presses < 256; t = strtok(NULL, ","))
            {
                char name[16];
                press_t *p = &presses[num_presses];
                p->length = 10;
                if (sscanf(t, "%d:%15[^:]:%d", &p->frame, name, &p->length) >= 2 && (p->id = button_id(name)) >= 0)
                    num_presses++;
                else
                    fprintf(stderr, "ignoring bad --press entry '%s'\n", t);
            }
            i++;
        }
        else
        {
            fprintf(stderr, "unknown option %s\n", a);
            return 2;
        }
    }

    void *core = dlopen(argv[1], RTLD_NOW);
    if (!core)
    {
        fprintf(stderr, "%s\n", dlerror());
        return 1;
    }
    void (*p_retro_set_environment)(retro_environment_t);
    void (*p_retro_set_video_refresh)(retro_video_refresh_t);
    void (*p_retro_set_audio_sample)(retro_audio_sample_t);
    void (*p_retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*p_retro_set_input_poll)(retro_input_poll_t);
    void (*p_retro_set_input_state)(retro_input_state_t);
    void (*p_retro_init)(void);
    bool (*p_retro_load_game)(const struct retro_game_info *);
    void (*p_retro_run)(void);
    void (*p_retro_set_controller_port_device)(unsigned, unsigned);
    SYM(retro_set_environment); SYM(retro_set_video_refresh); SYM(retro_set_audio_sample);
    SYM(retro_set_audio_sample_batch); SYM(retro_set_input_poll); SYM(retro_set_input_state);
    SYM(retro_init); SYM(retro_load_game); SYM(retro_run); SYM(retro_set_controller_port_device);

    p_retro_set_environment(environment);
    p_retro_init();
    p_retro_set_video_refresh(video);
    p_retro_set_audio_sample(audio);
    p_retro_set_audio_sample_batch(audio_batch);
    p_retro_set_input_poll(input_poll);
    p_retro_set_input_state(input_state);

    FILE *f = fopen(argv[2], "rb");
    if (!f)
    {
        perror(argv[2]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *rom = malloc(size);
    if (fread(rom, 1, size, f) != (size_t)size)
        return 1;
    fclose(f);

    struct retro_game_info info = { .path = argv[2], .data = rom, .size = (size_t)size };
    if (!p_retro_load_game(&info))
    {
        fprintf(stderr, "core refused %s\n", argv[2]);
        return 1;
    }
    p_retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

    for (int frame = 1; frame <= frames; frame++)
    {
        memset(pad, 0, sizeof pad);
        for (int i = 0; i < num_presses; i++)
            if (frame >= presses[i].frame && frame < presses[i].frame + presses[i].length)
                pad[presses[i].id] = true;
        p_retro_run();

        bool snap = snap_every && frame % snap_every == 0;
        for (int i = 0; i < num_snaps && !snap; i++)
            snap = snaps[i] == frame;
        if (hash && frame_rgb)
        {
            uint32_t h = 2166136261u;
            for (unsigned n = 0; n < frame_w * frame_h; n++)
                h = (h ^ frame_rgb[n]) * 16777619u;
            printf("frame %d %ux%u %08x\n", frame, frame_w, frame_h, h);
        }
        if (snap && frame_rgb)
        {
            char path[1024];
            snprintf(path, sizeof path, "%s/frame%05d.ppm", out_dir, frame);
            write_ppm(path);
        }
    }

    if (dump_path)
        picodrive_dump(core, dump_path);
    return 0;
}
