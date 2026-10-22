// The four POKEY sound chips, shared by the PC harness and the 32X build.
//
// Copyright 2026 the irobot 32X port authors.
// Adapted from the Quad POKEY in John Manfreda's I, Robot Project emulator
// (https://github.com/manfreda-dot-org/I-Robot-Project), GPL-3.0-or-later,
// which is based on Ron Fries' POKEY Chip Emulator 2.3. See LICENSE.
//
// Each channel counts POKEY clocks down to its next output change. Rather
// than stepping every clock, the renderer jumps straight to whichever comes
// first: a channel's change or the next output sample. The polynomial
// counters are not stepped either: the clocks since they were last brought
// up to date are counted, and they catch up only when a channel needs a bit.

#include <string.h>

#include "pokey.h"

#define POLY4_SIZE  15
#define POLY5_SIZE  31
#define POLY9_SIZE  511
#define POLY17_SIZE 131071

// Divisors from the POKEY clock to its 64 kHz and 15 kHz base clocks.
#define DIV_64      28
#define DIV_15      114

// A channel that never changes: silent, volume only, or above the sample
// rate. Its output is held high.
#define IDLE        0x7FFFFFFFu

// AUDC bits
#define AUDC_NOTPOLY5   0x80
#define AUDC_POLY4      0x40
#define AUDC_PURE       0x20
#define AUDC_VOL_ONLY   0x10

// AUDCTL bits
#define AUDCTL_POLY9    0x80
#define AUDCTL_CH1_179  0x40
#define AUDCTL_CH3_179  0x20
#define AUDCTL_CH1_CH2  0x10
#define AUDCTL_CH3_CH4  0x08
#define AUDCTL_CLOCK_15 0x01

static const uint8_t bit4[POLY4_SIZE] = { 1, 1, 0, 1, 1, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0 };
static const uint8_t bit5[POLY5_SIZE] = {
    0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1, 0, 1,
    0, 1, 1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 1 };
static uint32_t bit17[(POLY17_SIZE + 31) / 32];

typedef struct
{
    uint8_t audf[4], audc[4], audctl;
    uint8_t on[4];                  // output state of each channel
    uint32_t div_max[4];            // clocks between output changes
    uint32_t div_cnt[4];            // clocks until the next change
    uint32_t base_mult;             // DIV_64 or DIV_15
    uint32_t samp_cnt;              // clocks until the next sample, 16.16
    uint32_t p4, p5, p9, p17;       // polynomial counter positions
    uint32_t adjust;                // clocks since the positions were updated
    int level;                      // sum of the volumes of channels that are on
} chip_t;

static chip_t chips[4];
static uint32_t samp_max;           // clocks per sample, 16.16

static inline int poly17_bit(uint32_t n)
{
    return (bit17[n >> 5] >> (n & 31)) & 1;
}

// Advance a polynomial position. Division is slow on the SH-2, so for the
// usual short gaps the remainder comes from a multiply by the reciprocal,
// which is exact below 2^20 for these sizes, or from subtraction.
static inline uint32_t advance(uint32_t p, uint32_t clocks, uint32_t size, uint32_t recip)
{
    p += clocks;
    if (p >= 1u << 20)
        return p % size;
    if (recip)
        p -= (uint32_t)(((uint64_t)p * recip) >> 32) * size;
    while (p >= size)
        p -= size;
    return p;
}

#define RECIP(size) ((uint32_t)(0x100000000ull / (size)) + 1)

static void update_polys(chip_t *p)
{
    uint32_t a = p->adjust;
    p->adjust = 0;
    p->p4 = advance(p->p4, a, POLY4_SIZE, RECIP(POLY4_SIZE));
    p->p5 = advance(p->p5, a, POLY5_SIZE, RECIP(POLY5_SIZE));
    p->p9 = advance(p->p9, a, POLY9_SIZE, RECIP(POLY9_SIZE));
    p->p17 = advance(p->p17, a, POLY17_SIZE, 0);
}

// Hold a channel high if it will not change at an audible rate.
static void set_output(chip_t *p, int c)
{
    if ((p->audc[c] & AUDC_VOL_ONLY) || !(p->audc[c] & 0x0F) || p->div_max[c] < (samp_max >> 16))
    {
        p->on[c] = 1;
        p->div_max[c] = IDLE;
        p->div_cnt[c] = IDLE;
    }
}

// Recompute a channel's divider. As the data sheet gives it: AUDF + 1 at
// 64 or 15 kHz, AUDF + 4 at 1.79 MHz (here 1.512 MHz), and for a pair of
// channels joined into 16 bits at that clock, AUDF + 256 * AUDF' + 7.
static void update(chip_t *p, int c)
{
    uint32_t f = p->audf[c], m;
    uint8_t ctl = p->audctl;
    switch (c)
    {
    case 0:
        m = (ctl & AUDCTL_CH1_179) ? f + 4 : (f + 1) * p->base_mult;
        break;
    case 1:
        if (!(ctl & AUDCTL_CH1_CH2))
            m = (f + 1) * p->base_mult;
        else if (ctl & AUDCTL_CH1_179)
            m = f * 256 + p->audf[0] + 7;
        else
            m = (f * 256 + p->audf[0] + 1) * p->base_mult;
        break;
    case 2:
        m = (ctl & AUDCTL_CH3_179) ? f + 4 : (f + 1) * p->base_mult;
        break;
    default:
        if (!(ctl & AUDCTL_CH3_CH4))
            m = (f + 1) * p->base_mult;
        else if (ctl & AUDCTL_CH3_179)
            m = f * 256 + p->audf[2] + 7;
        else
            m = (f * 256 + p->audf[2] + 1) * p->base_mult;
        break;
    }
    if (p->div_max[c] != m)
    {
        p->div_max[c] = m;
        if (p->div_cnt[c] > m)
            p->div_cnt[c] = m;
    }
    set_output(p, c);
}

static void update_level(chip_t *p)
{
    int level = 0;
    for (int c = 0; c < 4; c++)
        if (p->on[c])
            level += p->audc[c] & 0x0F;
    p->level = level;
}

void ir_pokey_init(uint32_t sample_rate)
{
    uint32_t poly17 = 0;
    memset(bit17, 0, sizeof bit17);
    for (uint32_t i = 0; i < POLY17_SIZE; i++)
    {
        poly17 = (poly17 >> 1) + (~((poly17 << 16) ^ (poly17 << 11)) & 0x10000);
        if (poly17 & 0x100)
            bit17[i >> 5] |= 1u << (i & 31);
    }

    samp_max = (uint32_t)(((uint64_t)IR_POKEY_HZ << 16) / sample_rate);
    memset(chips, 0, sizeof chips);
    for (int n = 0; n < 4; n++)
    {
        chip_t *p = &chips[n];
        p->base_mult = DIV_64;
        p->samp_cnt = samp_max;
        for (int c = 0; c < 4; c++)
            p->div_max[c] = p->div_cnt[c] = IDLE;
    }
}

void ir_pokey_write(uint8_t reg, uint8_t data)
{
    // Address bits --rccrrr: cc picks the chip, r the register.
    chip_t *p = &chips[(reg >> 3) & 3];
    int r = (reg & 7) | ((reg & 0x20) >> 2);
    if (r < 8)
    {
        int c = r >> 1;
        if (r & 1)
            p->audc[c] = data;
        else
            p->audf[c] = data;
        update(p, c);
        if (!(r & 1) && (c == 0 || c == 2))
            update(p, c + 1);       // the high half of a joined pair uses it too
    }
    else if (r == 8)
    {
        p->audctl = data;
        p->base_mult = (data & AUDCTL_CLOCK_15) ? DIV_15 : DIV_64;
        for (int c = 0; c < 4; c++)
            update(p, c);
    }
    else
        return;
    update_level(p);
}

static inline int chip_idle(const chip_t *p)
{
    return (p->div_cnt[0] & p->div_cnt[1] & p->div_cnt[2] & p->div_cnt[3]) == IDLE;
}

static void render_chip(chip_t *p, int16_t *out, int n)
{
    uint32_t *cnt = p->div_cnt;
    while (n > 0)
    {
        // The next channel change (idle channels never come first in
        // practice: IDLE is far beyond any buffer).
        uint32_t t = cnt[0];
        int c = 0;
        if (cnt[1] <= t) { t = cnt[1]; c = 1; }
        if (cnt[2] <= t) { t = cnt[2]; c = 2; }
        if (cnt[3] <= t) { t = cnt[3]; c = 3; }

        // Emit the samples that come before it. A change wins a tie.
        uint32_t left = t, s, sc = p->samp_cnt;
        int16_t level = (int16_t)p->level;
        while (n > 0 && (s = sc >> 16) < left)
        {
            left -= s;
            sc = (sc & 0xFFFF) + samp_max;
            *out++ += level;
            n--;
        }
        uint32_t ev = t - left;
        if (n > 0)
        {
            sc -= left << 16;   // the change is due before the next sample
            ev = t;
        }
        p->samp_cnt = sc;
        p->adjust += ev;
        if (cnt[0] != IDLE) cnt[0] -= ev;
        if (cnt[1] != IDLE) cnt[1] -= ev;
        if (cnt[2] != IDLE) cnt[2] -= ev;
        if (cnt[3] != IDLE) cnt[3] -= ev;
        if (n == 0)
            break;

        cnt[c] += p->div_max[c];
        uint8_t audc = p->audc[c];
        int toggle = 0;
        if ((audc & (AUDC_NOTPOLY5 | AUDC_PURE)) != (AUDC_NOTPOLY5 | AUDC_PURE))
            update_polys(p);
        if ((audc & AUDC_NOTPOLY5) || bit5[p->p5])
        {
            if (audc & AUDC_PURE)
                toggle = 1;
            else if (audc & AUDC_POLY4)
                toggle = bit4[p->p4] != p->on[c];
            else if (p->audctl & AUDCTL_POLY9)
                toggle = poly17_bit(p->p9) != p->on[c];
            else
                toggle = poly17_bit(p->p17) != p->on[c];
        }
        if (toggle)
        {
            p->on[c] ^= 1;
            int vol = audc & 0x0F;
            p->level += p->on[c] ? vol : -vol;
        }
    }
}

void ir_pokey_render(int16_t *out, int n)
{
    // Chips where nothing changes contribute a constant level.
    int base = 0;
    for (int i = 0; i < 4; i++)
    {
        chip_t *p = &chips[i];
        if (!chip_idle(p))
            continue;
        base += p->level;
        p->adjust += (uint32_t)(((uint64_t)samp_max * (uint32_t)n + (p->samp_cnt & 0xFFFF)) >> 16);
        if (p->adjust >= 0x40000000)
            update_polys(p);
    }
    for (int i = 0; i < n; i++)
        out[i] = (int16_t)base;
    for (int i = 0; i < 4; i++)
        if (!chip_idle(&chips[i]))
            render_chip(&chips[i], out, n);
}
