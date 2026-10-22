// Sound for the 32X build.
//
// Copyright 2026 the irobot 32X port authors. GPL-3.0-or-later, see LICENSE.
//
// The master SH-2 queues each frame's POKEY writes with a time in output
// samples. The slave renders them through pokey.c in short chunks whenever
// it is waiting for work, a few chunks ahead, and DMA channel 1 feeds the
// chunks to the PWM, which requests a transfer per sample. The DMA-end
// interrupt only starts the next chunk, so sound never holds up drawing.
//
// The master's times run a fixed number of samples per frame. The slave
// plays them `delay` samples later, keeping enough queued that a frame's
// writes arrive before they are due, and nudges `delay` to follow drift
// between the frame rate and the PWM rate.

#include <stdbool.h>

#include "../common/irobot.h"
#include "../common/pokey.h"
#include "shared.h"

#define SOUND_RATE      22050
#define PWM_CYCLE       (23011361 / SOUND_RATE + 1)
#define CHUNK           128                 // samples per DMA transfer
#define NUM_CHUNKS      4                   // must be a power of two
#define FRAME_SAMPLES   368                 // samples per 32X frame, 22050 / 59.92
#define QUEUE_LEN       1024                // must be a power of two

// Queued writes, master -> slave. `queue_now` is the time the master has
// reached: the end of the last frame it queued.
static uint32_t queue_time[QUEUE_LEN];
static uint16_t queue_write[QUEUE_LEN];     // reg << 8 | data
static uint32_t queue_head, queue_tail, queue_now;
static bool queue_started;

void sound_queue_frame(void)
{
    static uint32_t frame_start;

    int count;
    const ir_sound_write_t *w = ir_sound_writes(&count);
    uint32_t cycles = ir_frame_cycles();
    uint32_t step = cycles ? ((uint32_t)FRAME_SAMPLES << 16) / cycles : 0;   // 16.16 per cycle

    uint32_t head = *UNCACHED(&queue_head);
    uint32_t tail = *UNCACHED(&queue_tail);
    for (int i = 0; i < count; i++)
    {
        uint32_t next = (head + 1) & (QUEUE_LEN - 1);
        if (next == tail)
            break;
        *UNCACHED(&queue_time[head]) = frame_start + ((w[i].time * step) >> 16);
        *UNCACHED(&queue_write[head]) = (uint16_t)(w[i].reg << 8 | w[i].data);
        head = next;
    }
    frame_start += FRAME_SAMPLES;
    *UNCACHED(&queue_head) = head;
    *UNCACHED(&queue_now) = frame_start;
    *UNCACHED(&queue_started) = true;
}

// Slave side.

static uint32_t pwm_buffer[NUM_CHUNKS][CHUNK];  // left << 16 | right
static uint32_t pwm_hold[CHUNK];            // played if rendering falls behind
static volatile uint32_t rendered, played;  // chunk counts; `played` is playing
volatile uint32_t sound_underruns;          // for a debugger
static int16_t mix[CHUNK];
static uint32_t mix_pos;                    // time of the chunk being rendered
static int32_t delay;
static bool synced;
static uint32_t pwm_level[IR_POKEY_MAX + 1];   // POKEY level -> PWM word

static void start_dma(const uint32_t *buffer)
{
    SH2_DMA_SAR1 = (uint32_t)buffer | 0x20000000;   // cache-through
    SH2_DMA_TCR1 = CHUNK;
    // Fixed destination, incrementing source, longs, DREQ on the rising
    // edge, cycle steal, dual address, interrupt at the end, enabled.
    SH2_DMA_CHCR1 = 0x18E5;
}

static void render(uint32_t *out)
{
    if (*UNCACHED(&queue_started))
    {
        int32_t slack = (int32_t)(*UNCACHED(&queue_now) + delay - mix_pos);
        if (!synced || slack < 0 || slack > 6 * FRAME_SAMPLES)
        {
            delay = (int32_t)(mix_pos - *UNCACHED(&queue_now)) + CHUNK + FRAME_SAMPLES;
            synced = true;
        }
        else if (slack < CHUNK)
            delay++;
        else if (slack > 2 * CHUNK + 2 * FRAME_SAMPLES)
            delay--;
    }

    uint32_t head = *UNCACHED(&queue_head);
    uint32_t tail = *UNCACHED(&queue_tail);
    int at = 0;
    while (synced && tail != head)
    {
        int32_t pos = (int32_t)(*UNCACHED(&queue_time[tail]) + delay - mix_pos);
        if (pos >= CHUNK && pos < 8 * FRAME_SAMPLES)
            break;
        if (pos >= CHUNK)
            pos = at;       // nonsense time: play it now rather than stall
        if (pos > at)
        {
            ir_pokey_render(mix + at, pos - at);
            at = pos;
        }
        uint16_t w = *UNCACHED(&queue_write[tail]);
        ir_pokey_write((uint8_t)(w >> 8), (uint8_t)w);
        tail = (tail + 1) & (QUEUE_LEN - 1);
    }
    if (at < CHUNK)
        ir_pokey_render(mix + at, CHUNK - at);
    *UNCACHED(&queue_tail) = tail;
    mix_pos += CHUNK;

    // The level straight to PWM. The 32X output is AC coupled, like the
    // arcade amplifier, so there is no need to center it.
    for (int i = 0; i < CHUNK; i++)
        out[i] = pwm_level[mix[i]];
}

static volatile bool rendering;     // keeps the interrupt out of sound_service

void sound_service(void)
{
    rendering = true;
    uint32_t next = rendered + 1;
    if (next - played < NUM_CHUNKS)
    {
        render(pwm_buffer[next & (NUM_CHUNKS - 1)]);
        rendered = next;
    }
    rendering = false;
}

// DMA channel 1 end interrupt (mars_start.s).
void slave_dma1(void)
{
    (void)SH2_DMA_CHCR1;    // read TE before clearing it
    SH2_DMA_CHCR1 = 0;
    if (rendered != played)
        start_dma(pwm_buffer[++played & (NUM_CHUNKS - 1)]);
    else
    {
        start_dma(pwm_hold);
        sound_underruns++;
    }
    // The slave has been busy drawing for a while: render the next chunk
    // now rather than let the sound run out.
    if (rendered == played && !rendering)
        sound_service();
}

void sound_init_slave(void)
{
    ir_pokey_init(SOUND_RATE);
    for (int i = 0; i <= IR_POKEY_MAX; i++)
    {
        uint32_t v = 1 + (uint32_t)i * 10;
        if (v > PWM_CYCLE - 1)
            v = PWM_CYCLE - 1;
        pwm_level[i] = v << 16 | v;
    }
    for (int i = 0; i < CHUNK; i++)
        pwm_buffer[0][i] = pwm_hold[i] = pwm_level[0];

    MARS_PWM_CTRL = 0x0185;     // timer every sample, DREQ, left and right out
    MARS_PWM_CYCLE = PWM_CYCLE;

    SH2_DMA_CHCR1 = 0;
    SH2_DMA_DRCR1 = 0;          // DREQ1 is the PWM
    SH2_DMA_DAR1 = 0x20004034;  // PWM left and right
    SH2_DMA_DMAOR = 1;
    SH2_DMA_VCR1 = 66;          // the level 4 vector, unused by the 32X
    SH2_INT_IPRA = (SH2_INT_IPRA & 0xF0FF) | 0x0400;

    rendered = played = 0;
    start_dma(pwm_buffer[0]);
}
