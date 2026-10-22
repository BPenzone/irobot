// 32X video backend. The master SH-2 runs the machine and fills display
// lists; the slave SH-2 rasterizes them into the frame buffer and flips.
//
// Copyright 2026 the irobot 32X port authors. GPL-3.0-or-later, see LICENSE.

#include <string.h>

#include "../common/internal.h"
#include "shared.h"

#define NUM_LISTS   3
#define ALPHA_PEN   (IR_PIXEL_BASE + 64)    // first of the four alphanumeric colors
#define QUEUE_LEN   16

typedef struct
{
    uint32_t op, arg, used;
} slave_cmd_t;

static int32_t lists[NUM_LISTS][IR_DLIST_WORDS];

// Accessed only through UNCACHED().
static uint32_t list_busy[NUM_LISTS];
static slave_cmd_t queue[QUEUE_LEN];
static uint32_t queue_head, queue_tail;

// Master side: palette shadow written to CRAM during VBlank.
static uint16_t palette[64];
static volatile int palette_dirty;
static volatile uint32_t vblanks;

static ir_stats_t slave_stats;

static void push(uint32_t op, uint32_t arg, uint32_t used)
{
    uint32_t head = *UNCACHED(&queue_head);
    while (((head + 1) & (QUEUE_LEN - 1)) == *UNCACHED(&queue_tail))
        ;
    volatile slave_cmd_t *c = UNCACHED(&queue[head]);
    c->op = op;
    c->arg = arg;
    c->used = used;
    *UNCACHED(&queue_head) = (head + 1) & (QUEUE_LEN - 1);
}

void ir_video_reset(void)
{
    memset(palette, 0, sizeof palette);
    palette_dirty = 1;
}

int32_t *ir_video_submit(int target, int32_t *words, uint32_t used, ir_stats_t *stats)
{
    (void)target;   // the slave always draws into the hidden frame buffer
    (void)stats;
    int next = 0;
    if (words)
    {
        int index = (int)((words - lists[0]) / IR_DLIST_WORDS);
        *UNCACHED(&list_busy[index]) = 1;
        push(SLAVE_DRAW, (uint32_t)index, used);
        next = index + 1 == NUM_LISTS ? 0 : index + 1;
    }
    while (*UNCACHED(&list_busy[next]))
        ;
    return lists[next];
}

void ir_video_show(int shown)
{
    (void)shown;
    push(SLAVE_FLIP, 0, 0);
}

void ir_video_set_color(int index, uint8_t r, uint8_t g, uint8_t b)
{
    palette[index] = (uint16_t)COLOR(r >> 3, g >> 3, b >> 3);
    palette_dirty = 1;
}

// Called from the master's VBlank interrupt (mars_start.s).
void master_vblank(void)
{
    vblanks++;
    if (palette_dirty)
    {
        volatile uint16_t *cram = &MARS_CRAM;
        palette_dirty = 0;
        for (int n = 0; n < 64; n++)
            cram[IR_PIXEL_BASE + n] = palette[n];
        const uint32_t *alpha = ir_alpha_palette();
        for (int n = 0; n < 4; n++)
            cram[ALPHA_PEN + n] = (uint16_t)COLOR((alpha[n] >> 19) & 31, (alpha[n] >> 11) & 31, (alpha[n] >> 3) & 31);
    }
}

uint32_t master_vblank_count(void)
{
    return vblanks;
}

// ---------------------------------------------------------------------------
// Slave side
// ---------------------------------------------------------------------------

// The alphanumerics are an overlay on the arcade, but here they are drawn
// into the frame buffer. The pixels under each character are saved so they
// can be put back before the slave draws into that buffer again; otherwise
// text would stay behind wherever the game doesn't redraw or erase (the
// game fills the screen with block characters between scenes). The old
// pixels go in the unused end of the same frame buffer.
#define ALPHA_SAVE_CELLS  ((IR_SCREEN_H / 8) * 32)
#define ALPHA_SAVE_START  (FB_FIRST_LINE + IR_SCREEN_H * FB_LINE_BYTES)
#define ALPHA_SAVE_PIXELS (0x20000 - ALPHA_SAVE_START)

typedef struct
{
    uint16_t cell[ALPHA_SAVE_CELLS];    // row * 32 + col
    uint8_t ch[ALPHA_SAVE_CELLS];
    int cells;
} alpha_save_t;

static alpha_save_t alpha_save[2];

static uint8_t *cell_pixels(uint8_t *fb, int cell)
{
    return fb + ((cell >> 5) * 8) * FB_LINE_BYTES + (cell & 31) * 8;
}

static uint8_t *saved_pixels(void)
{
    return (uint8_t *)&MARS_FRAMEBUFFER + ALPHA_SAVE_START;
}

static void draw_alpha(uint8_t *fb, alpha_save_t *save)
{
    uint8_t *pixels = saved_pixels();
    const uint8_t *ram = (const uint8_t *)UNCACHED(ir_alpha_ram());
    const uint8_t *charset = ir_alpha_charset();
    int used = 0;
    save->cells = 0;
    for (int cell = 0; cell < (IR_SCREEN_H / 8) * 32; cell++)
    {
        uint8_t v = ram[cell];
        int ch = v & 0x3F;
        if (!ch)
            continue;
        const uint8_t *glyph = &charset[ch * 8];
        bool keep = save->cells < ALPHA_SAVE_CELLS && used + 64 <= ALPHA_SAVE_PIXELS;
        if (keep)
        {
            save->cell[save->cells] = (uint16_t)cell;
            save->ch[save->cells++] = (uint8_t)ch;
        }
        uint8_t color = (uint8_t)(ALPHA_PEN + (v >> 6));
        uint8_t *dst = cell_pixels(fb, cell);
        for (int y = 0; y < 8; y++, dst += FB_LINE_BYTES)
        {
            unsigned bits = glyph[y];
            for (int x = 0; bits; x++, bits = (bits << 1) & 0xFF)
                if (bits & 0x80)
                {
                    if (keep)
                        pixels[used++] = dst[x];
                    dst[x] = color;
                }
        }
    }
}

static void erase_alpha(uint8_t *fb, alpha_save_t *save)
{
    const uint8_t *pixels = saved_pixels();
    const uint8_t *charset = ir_alpha_charset();
    int used = 0;
    for (int n = 0; n < save->cells; n++)
    {
        const uint8_t *glyph = &charset[save->ch[n] * 8];
        uint8_t *dst = cell_pixels(fb, save->cell[n]);
        for (int y = 0; y < 8; y++, dst += FB_LINE_BYTES)
        {
            unsigned bits = glyph[y];
            for (int x = 0; bits; x++, bits = (bits << 1) & 0xFF)
                if (bits & 0x80)
                    dst[x] = pixels[used++];
        }
    }
    save->cells = 0;
}

void slave_main_loop(void)
{
    uint8_t *fb = (uint8_t *)&MARS_FRAMEBUFFER + FB_FIRST_LINE + FB_X_OFFSET;
    for (;;)
    {
        uint32_t tail = *UNCACHED(&queue_tail);
        while (*UNCACHED(&queue_head) == tail)
            sound_service();
        volatile slave_cmd_t *c = UNCACHED(&queue[tail]);
        slave_cmd_t cmd = { c->op, c->arg, c->used };

        if (cmd.op == SLAVE_DRAW)
        {
            CacheControl(SH2_CCTL_CP | SH2_CCTL_CE);    // drop stale list data
            ir_raster_list(fb, FB_LINE_BYTES, lists[cmd.arg], cmd.used, &slave_stats);
            *UNCACHED(&list_busy[cmd.arg]) = 0;
        }
        else if (cmd.op == SLAVE_FLIP)
        {
            int fs = MARS_VDP_FBCTL & MARS_VDP_FS;
            draw_alpha(fb, &alpha_save[fs]);
            MARS_VDP_FBCTL = (uint16_t)(fs ^ 1);
            while ((MARS_VDP_FBCTL & MARS_VDP_FS) == fs)
                sound_service();
            erase_alpha(fb, &alpha_save[fs ^ 1]);
        }
        *UNCACHED(&queue_tail) = (tail + 1) & (QUEUE_LEN - 1);
    }
}
