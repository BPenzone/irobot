// Differential test of the recompiler against the interpreter: runs random
// single instructions both ways from the same random state and compares
// registers, flags, cycle counts and memory.
//
// Copyright 2026 the irobot 32X port authors. GPL-3.0-or-later, see LICENSE.
//
// Build and run with `make recomp-selftest`.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/common/rc_runtime.h"

void mc6809_reset(mc6809__t *cpu);
MC6809_RESULT mc6809_step(mc6809__t *cpu);

extern const rc_selftest_t rc_selftests[];
extern const int rc_selftest_count;

mc6809__t ir_cpu;
uint8_t ir_prog_bank;
uint8_t rc_test_mem[0x10000];
const rc_block_t rc_fixed[0xA000];
const rc_block_t rc_banked[6][0x2000];

static uint32_t rng = 12345;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static void describe(const char *tag, const mc6809__t *c)
{
    printf("  %s: PC=%04X A=%02X B=%02X X=%04X Y=%04X U=%04X S=%04X DP=%02X "
           "E%dF%dH%dI%dN%dZ%dV%dC%d clk=%u nmi=%d\n", tag, c->PC.w, c->A, c->B, c->X.w, c->Y.w,
           c->U.w, c->S.w, c->DP, c->CC_E, c->CC_F, c->CC_H, c->CC_I, c->CC_N, c->CC_Z, c->CC_V,
           c->CC_C, c->Clock, c->NMI_Armed);
}

static bool same_regs(const mc6809__t *a, const mc6809__t *b)
{
    return a->PC.w == b->PC.w && a->D.w == b->D.w && a->X.w == b->X.w && a->Y.w == b->Y.w &&
           a->U.w == b->U.w && a->S.w == b->S.w && a->DP == b->DP && a->CC_E == b->CC_E &&
           a->CC_F == b->CC_F && a->CC_H == b->CC_H && a->CC_I == b->CC_I && a->CC_N == b->CC_N &&
           a->CC_Z == b->CC_Z && a->CC_V == b->CC_V && a->CC_C == b->CC_C &&
           a->Clock == b->Clock && a->NMI_Armed == b->NMI_Armed;
}

int main(void)
{
    static uint8_t mem0[0x10000], mem1[0x10000];
    int failures = 0;

    for (int p = 0; p < 256; p++)
    {
        ir_cpu.ReadPointer[p] = rc_test_mem + p * 256;
        ir_cpu.WritePointer[p] = rc_test_mem + p * 256;
    }

    for (int n = 0; n < rc_selftest_count; n++)
    {
        const rc_selftest_t *t = &rc_selftests[n];
        for (int rep = 0; rep < 8; rep++)
        {
            for (int i = 0; i < 0x10000; i += 4)
            {
                uint32_t v = rnd();
                memcpy(&mem0[i], &v, 4);
            }
            memcpy(&mem0[0x1000], t->code, (size_t)t->length);

            mc6809__t start = ir_cpu;
            start.PC.w = 0x1000;
            start.D.w = (uint16_t)rnd();
            start.X.w = (uint16_t)rnd();
            start.Y.w = (uint16_t)rnd();
            start.U.w = (uint16_t)rnd();
            start.S.w = (uint16_t)rnd();
            start.DP = (uint8_t)rnd();
            uint8_t cc = (uint8_t)rnd();
            start.CC_C = cc & 1; start.CC_V = (cc >> 1) & 1; start.CC_Z = (cc >> 2) & 1;
            start.CC_N = (cc >> 3) & 1; start.CC_I = (cc >> 4) & 1; start.CC_H = (cc >> 5) & 1;
            start.CC_F = (cc >> 6) & 1; start.CC_E = (cc >> 7) & 1;
            start.Clock = rnd();
            start.NMI_Armed = rnd() & 1;
            start.IRQ = start.FIRQ = start.NMI = false;
            start.CWAI = start.SYNC = false;

            // Interpreter
            ir_cpu = start;
            memcpy(rc_test_mem, mem0, sizeof mem0);
            mc6809_step(&ir_cpu);
            mc6809__t want = ir_cpu;
            memcpy(mem1, rc_test_mem, sizeof mem1);

            // Recompiled
            ir_cpu = start;
            memcpy(rc_test_mem, mem0, sizeof mem0);
            ir_cpu.Limit = start.Clock + 1000;
            void *next = t->fn();
            mc6809__t got = ir_cpu;

            bool ok = !next && same_regs(&want, &got) && !memcmp(mem1, rc_test_mem, sizeof mem1);
            if (!ok)
            {
                if (failures < 400)
                {
                    printf("MISMATCH %s [", t->text);
                    for (int i = 0; i < t->length; i++)
                        printf(" %02X", t->code[i]);
                    printf(" ]\n");
                    describe("start", &start);
                    describe("interp", &want);
                    describe("recomp", &got);
                    for (int i = 0; i < 0x10000; i++)
                        if (mem1[i] != rc_test_mem[i])
                        {
                            printf("  mem %04X: interp %02X recomp %02X\n", i, mem1[i], rc_test_mem[i]);
                            break;
                        }
                }
                failures++;
                break;
            }
        }
    }
    printf("%d of %d instruction cases failed\n", failures, rc_selftest_count);
    return failures != 0;
}
