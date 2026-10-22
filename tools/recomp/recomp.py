#!/usr/bin/env python3
"""Static recompiler: translates the I, Robot 6809 program into C.

Copyright 2026 the irobot 32X port authors. GPL-3.0-or-later, see LICENSE.

Usage:
  recomp.py IMAGE OUT.c [--hints FILE] [--exit-at ADDR ...]
  recomp.py --selftest OUT.c [--seed N] [--count N]

IMAGE is the prepared ROM image (build/.../irobot.img, layout ir_image_t).
The output is a C file with one function per basic block, plus lookup
tables from (bank, address) to block. It runs against the same CPU state
(mc6809__t) as the interpreter, so the machine can switch between the two
at any block boundary; anything the recompiler cannot or does not translate
runs in the interpreter instead.

--selftest writes random single-instruction blocks for differential testing
against the interpreter (tools/recomp/selftest.c).

Block functions return the next block to run, or NULL to hand control back
to the machine loop with ir_cpu.PC set. Every block starts by checking the
cycle budget, which is also how the machine forces an early exit.
"""

import argparse
import random
import sys

FIXED_BASE = 0x6000
BANK_BASE = 0x4000
BANK_SIZE = 0x2000
NUM_BANKS = 6

# ---------------------------------------------------------------------------
# Opcode table: (mnemonic, mode, cycles) with MC6809E datasheet cycle counts.
# Modes: inh imm8 imm16 dir ext idx rel8 rel16 psh pul tfr
# ---------------------------------------------------------------------------

P1 = {}
P2 = {}
P3 = {}


def op(table, code, mn, mode, cyc):
    table[code] = (mn, mode, cyc)


for base, mode, cyc, jmp in ((0x00, 'dir', 6, 3), (0x60, 'idx', 6, 3), (0x70, 'ext', 7, 4)):
    for off, mn in ((0x0, 'NEG'), (0x3, 'COM'), (0x4, 'LSR'), (0x6, 'ROR'), (0x7, 'ASR'),
                    (0x8, 'ASL'), (0x9, 'ROL'), (0xA, 'DEC'), (0xC, 'INC'), (0xD, 'TST'),
                    (0xF, 'CLR')):
        op(P1, base + off, mn, mode, cyc)
    op(P1, base + 0xE, 'JMP', mode, jmp)

for base, reg in ((0x40, 'A'), (0x50, 'B')):
    for off, mn in ((0x0, 'NEG'), (0x3, 'COM'), (0x4, 'LSR'), (0x6, 'ROR'), (0x7, 'ASR'),
                    (0x8, 'ASL'), (0x9, 'ROL'), (0xA, 'DEC'), (0xC, 'INC'), (0xD, 'TST'),
                    (0xF, 'CLR')):
        op(P1, base + off, mn + reg, 'inh', 2)

op(P1, 0x12, 'NOP', 'inh', 2)
op(P1, 0x13, 'SYNC', 'inh', 4)
op(P1, 0x16, 'LBRA', 'rel16', 5)
op(P1, 0x17, 'LBSR', 'rel16', 9)
op(P1, 0x19, 'DAA', 'inh', 2)
op(P1, 0x1A, 'ORCC', 'imm8', 3)
op(P1, 0x1C, 'ANDCC', 'imm8', 3)
op(P1, 0x1D, 'SEX', 'inh', 2)
op(P1, 0x1E, 'EXG', 'tfr', 8)
op(P1, 0x1F, 'TFR', 'tfr', 6)

BRANCHES = ['BRA', 'BRN', 'BHI', 'BLS', 'BCC', 'BCS', 'BNE', 'BEQ',
            'BVC', 'BVS', 'BPL', 'BMI', 'BGE', 'BLT', 'BGT', 'BLE']
for n, mn in enumerate(BRANCHES):
    op(P1, 0x20 + n, mn, 'rel8', 3)
    if n:
        op(P2, 0x20 + n, 'L' + mn, 'rel16', 5)

op(P1, 0x30, 'LEAX', 'idx', 4)
op(P1, 0x31, 'LEAY', 'idx', 4)
op(P1, 0x32, 'LEAS', 'idx', 4)
op(P1, 0x33, 'LEAU', 'idx', 4)
op(P1, 0x34, 'PSHS', 'psh', 5)
op(P1, 0x35, 'PULS', 'pul', 5)
op(P1, 0x36, 'PSHU', 'psh', 5)
op(P1, 0x37, 'PULU', 'pul', 5)
op(P1, 0x39, 'RTS', 'inh', 5)
op(P1, 0x3A, 'ABX', 'inh', 3)
op(P1, 0x3B, 'RTI', 'inh', 6)
op(P1, 0x3C, 'CWAI', 'imm8', 20)
op(P1, 0x3D, 'MUL', 'inh', 11)
op(P1, 0x3F, 'SWI', 'inh', 19)

# Accumulator group: 8x imm, 9x dir, Ax idx, Bx ext (A); Cx..Fx (B / D / U)
ACC = [(0x0, 'SUB'), (0x1, 'CMP'), (0x2, 'SBC'), (0x4, 'AND'), (0x5, 'BIT'), (0x6, 'LD'),
       (0x7, 'ST'), (0x8, 'EOR'), (0x9, 'ADC'), (0xA, 'OR'), (0xB, 'ADD')]
for hi, reg in ((0x80, 'A'), (0xC0, 'B')):
    for col, mode, cyc in ((0x00, 'imm8', 2), (0x10, 'dir', 4), (0x20, 'idx', 4), (0x30, 'ext', 5)):
        for off, mn in ACC:
            if mn == 'ST' and mode == 'imm8':
                continue
            op(P1, hi + col + off, mn + reg, mode, cyc)

for col, mode, c16, cjsr, cld in ((0x00, 'imm16', 4, None, 3), (0x10, 'dir', 6, 7, 5),
                                  (0x20, 'idx', 6, 7, 5), (0x30, 'ext', 7, 8, 6)):
    op(P1, 0x83 + col, 'SUBD', mode, c16)
    op(P1, 0x8C + col, 'CMPX', mode, c16)
    op(P1, 0x8E + col, 'LDX', mode, cld)
    op(P1, 0xC3 + col, 'ADDD', mode, c16)
    op(P1, 0xCC + col, 'LDD', mode, cld)
    op(P1, 0xCE + col, 'LDU', mode, cld)
    if mode != 'imm16':
        op(P1, 0x8D + col, 'JSR', mode, cjsr)
        op(P1, 0x8F + col, 'STX', mode, cld)
        op(P1, 0xCD + col, 'STD', mode, cld)
        op(P1, 0xCF + col, 'STU', mode, cld)
op(P1, 0x8D, 'BSR', 'rel8', 7)

op(P2, 0x3F, 'SWI2', 'inh', 20)
op(P3, 0x3F, 'SWI3', 'inh', 20)
for col, mode, c16, cld in ((0x00, 'imm16', 5, 4), (0x10, 'dir', 7, 6),
                            (0x20, 'idx', 7, 6), (0x30, 'ext', 8, 7)):
    op(P2, 0x83 + col, 'CMPD', mode, c16)
    op(P2, 0x8C + col, 'CMPY', mode, c16)
    op(P2, 0x8E + col, 'LDY', mode, cld)
    op(P2, 0xCE + col, 'LDS', mode, cld)
    op(P3, 0x83 + col, 'CMPU', mode, c16)
    op(P3, 0x8C + col, 'CMPS', mode, c16)
    if mode != 'imm16':
        op(P2, 0x8F + col, 'STY', mode, cld)
        op(P2, 0xCF + col, 'STS', mode, cld)

# Instructions left to the interpreter: they wait for interrupts or are rare
# enough that translating them buys nothing.
INTERPRET = {'SYNC', 'CWAI', 'SWI', 'SWI2', 'SWI3', 'RTI'}

REG16 = {0: 'D', 1: 'X', 2: 'Y', 3: 'U', 4: 'S', 5: 'PC'}
REG8 = {8: 'A', 9: 'B', 10: 'CC', 11: 'DP'}
IDXREG = ['X', 'Y', 'U', 'S']


class Insn:
    pass


def s8(v):
    return v - 0x100 if v & 0x80 else v


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def decode(mem, addr):
    """Decode one instruction. mem(addr) returns a byte or None."""
    i = Insn()
    i.addr = addr
    b = mem(addr)
    if b is None:
        return None
    pc = addr + 1
    table = P1
    i.prefix = 0
    if b in (0x10, 0x11):
        table = P2 if b == 0x10 else P3
        i.prefix = b
        b = mem(pc)
        if b is None:
            return None
        pc += 1
    if b not in table:
        return None
    i.opcode = b
    i.mn, i.mode, i.cycles = table[b]

    def byte():
        nonlocal pc
        v = mem(pc)
        pc += 1
        return v

    def word():
        hi, lo = byte(), byte()
        if hi is None or lo is None:
            return None
        return hi << 8 | lo

    i.post = None
    i.value = None
    if i.mode in ('imm8', 'dir', 'psh', 'pul', 'tfr'):
        i.value = byte()
        if i.value is None:
            return None
    elif i.mode in ('imm16', 'ext'):
        i.value = word()
        if i.value is None:
            return None
    elif i.mode == 'rel8':
        v = byte()
        if v is None:
            return None
        i.value = (pc + s8(v)) & 0xFFFF
    elif i.mode == 'rel16':
        v = word()
        if v is None:
            return None
        i.value = (pc + v) & 0xFFFF
    elif i.mode == 'idx':
        post = byte()
        if post is None:
            return None
        i.post = post
        i.idx = decode_indexed(post, byte, word)
        if i.idx is None:
            return None
        i.cycles += i.idx['cycles']
    if i.mode in ('psh', 'pul'):
        n = 0
        for bit in range(8):
            if i.value & (1 << bit):
                n += 2 if bit >= 4 else 1
        i.cycles += n
    i.next = pc & 0xFFFF
    i.length = pc - addr
    if i.mode == 'tfr' and not tfr_valid(i.value):
        return None
    return i


def tfr_valid(post):
    s, d = post >> 4, post & 15
    if (s & 8) != (d & 8):
        return False
    regs = REG8 if s & 8 else REG16
    return s in regs and d in regs


def decode_indexed(post, byte, word):
    reg = IDXREG[(post >> 5) & 3]
    if post < 0x80:
        off = post & 0x1F
        if off & 0x10:
            off -= 0x20
        return {'kind': 'off', 'reg': reg, 'off': off, 'ind': False, 'cycles': 1}
    ind = bool(post & 0x10)
    m = post & 0x0F
    d = {'reg': reg, 'ind': ind}
    if m == 0x0 and not ind:
        d.update(kind='inc1', cycles=2)
    elif m == 0x1:
        d.update(kind='inc2', cycles=6 if ind else 3)
    elif m == 0x2 and not ind:
        d.update(kind='dec1', cycles=2)
    elif m == 0x3:
        d.update(kind='dec2', cycles=6 if ind else 3)
    elif m == 0x4:
        d.update(kind='zero', cycles=3 if ind else 0)
    elif m == 0x5:
        d.update(kind='accB', cycles=4 if ind else 1)
    elif m == 0x6:
        d.update(kind='accA', cycles=4 if ind else 1)
    elif m == 0x8:
        v = byte()
        if v is None:
            return None
        d.update(kind='off', off=s8(v), cycles=4 if ind else 1)
    elif m == 0x9:
        v = word()
        if v is None:
            return None
        d.update(kind='off', off=s16(v), cycles=7 if ind else 4)
    elif m == 0xB:
        d.update(kind='accD', cycles=7 if ind else 4)
    elif m == 0xC:
        v = byte()
        if v is None:
            return None
        d.update(kind='pc8', off=s8(v), cycles=4 if ind else 1)
    elif m == 0xD:
        v = word()
        if v is None:
            return None
        d.update(kind='pc16', off=s16(v), cycles=8 if ind else 5)
    elif m == 0xF and ind and ((post >> 5) & 3) == 0:
        v = word()
        if v is None:
            return None
        d.update(kind='abs', off=v, cycles=5)
    else:
        return None
    return d


# ---------------------------------------------------------------------------
# Code memory
# ---------------------------------------------------------------------------

class Image:
    def __init__(self, data):
        self.banked = [data[b * BANK_SIZE:(b + 1) * BANK_SIZE] for b in range(NUM_BANKS)]
        self.fixed = data[NUM_BANKS * BANK_SIZE:NUM_BANKS * BANK_SIZE + 0xA000]

    def reader(self, ctx):
        """Byte reader for code context ctx: 'F' (fixed) or a bank number."""
        def mem(a):
            a &= 0xFFFF
            if a >= FIXED_BASE:
                return self.fixed[a - FIXED_BASE]
            if a >= BANK_BASE and ctx != 'F':
                return self.banked[ctx][a - BANK_BASE]
            return None
        return mem

    def rom_byte(self, a):
        """A byte that is the same in every bank, or None."""
        if a >= FIXED_BASE:
            return self.fixed[a - FIXED_BASE]
        return None


def region(addr):
    if addr >= FIXED_BASE:
        return 'F'
    if addr >= BANK_BASE:
        return 'B'
    return None


# ---------------------------------------------------------------------------
# Discovery
# ---------------------------------------------------------------------------

def static_targets(i):
    """Statically known control-flow successors other than fall-through."""
    if i.mn in ('JMP', 'JSR') and i.mode == 'ext':
        return [i.value]
    if i.mode in ('rel8', 'rel16') and i.mn not in ('BRN', 'LBRN'):
        return [i.value]
    return []


def ends_block(i):
    """True if execution never falls through to the next instruction."""
    if i.mn in ('BRA', 'LBRA', 'JMP', 'RTS'):
        return True
    if i.mn in ('PULS', 'PULU') and i.value & 0x80:
        return True
    if i.mn in ('TFR', 'EXG') and (i.value & 0x0F) == 5:
        return True
    if i.mn in ('TFR', 'EXG') and (i.value >> 4) == 5 and i.mn == 'EXG':
        return True
    return False


def needs_exit_after(i):
    """Instructions after which pending interrupts must be checked."""
    if i.mn in ('ANDCC', 'ORCC'):
        return True
    if i.mn in ('PULS', 'PULU') and i.value & 0x01:
        return True
    if i.mn in ('TFR', 'EXG') and ((i.value & 0x0F) == 10 or (i.mn == 'EXG' and (i.value >> 4) == 10)):
        return True
    return False


def ctxs_for(addr, from_ctx):
    r = region(addr)
    if r == 'F':
        return ['F']
    if r == 'B':
        return [from_ctx] if from_ctx != 'F' else list(range(NUM_BANKS))
    return []


def discover(image, entries):
    """Returns {(ctx, addr): Insn} and the set of block leaders."""
    insns = {}
    leaders = set()
    work = []
    for ctx, a in entries:
        for c in ctxs_for(a, ctx):
            leaders.add((c, a))
            work.append((c, a))
    while work:
        ctx, a = work.pop()
        while True:
            if (ctx, a) in insns:
                break
            i = decode(image.reader(ctx), a)
            if i is None or i.mn in INTERPRET:
                break
            insns[(ctx, a)] = i
            for t in static_targets(i):
                for c in ctxs_for(t, ctx):
                    leaders.add((c, t))
                    work.append((c, t))
            if ends_block(i):
                break
            nxt = i.next
            if region(nxt) is None or (region(nxt) != region(a)):
                break
            if i.mode in ('rel8', 'rel16') or i.mn in ('JSR', 'BSR', 'LBSR') or needs_exit_after(i):
                leaders.add((ctx, nxt))
            a = nxt
    return insns, leaders


# ---------------------------------------------------------------------------
# C generation
# ---------------------------------------------------------------------------

def fname(ctx, addr):
    return 'B_%s_%04X' % (ctx, addr)


class Gen:
    def __init__(self, image, insns, leaders, exit_at, fold_rom=True):
        self.image = image
        self.insns = insns
        self.leaders = leaders
        self.exit_at = exit_at
        self.fold_rom = fold_rom
        self.out = []

    def emit(self, s):
        self.out.append(s)

    # Memory access ------------------------------------------------------

    def rd8(self, ea):
        """ea is a C expression or an int constant."""
        if isinstance(ea, int):
            ea &= 0xFFFF
            if self.fold_rom and self.image and self.image.rom_byte(ea) is not None:
                return '0x%02X' % self.image.rom_byte(ea)
            if ea < 0x0800:
                return 'ir_ram0[0x%04X]' % ea
            return 'RD(0x%04X)' % ea
        return 'RD(%s)' % ea

    def rd16(self, ea):
        if isinstance(ea, int):
            return '(uint16_t)(%s << 8 | %s)' % (self.rd8(ea), self.rd8(ea + 1))
        return 'RD16(%s)' % ea

    def wr8(self, ea, v):
        if isinstance(ea, int):
            ea &= 0xFFFF
            if ea < 0x0800:
                return 'ir_ram0[0x%04X] = %s;' % (ea, v)
            return 'WR(0x%04X, %s);' % (ea, v)
        return 'WR(%s, %s);' % (ea, v)

    def wr16(self, ea, v):
        if isinstance(ea, int):
            return '%s %s' % (self.wr8(ea, '(uint8_t)((%s) >> 8)' % v), self.wr8(ea + 1, '(uint8_t)(%s)' % v))
        return 'WR16(%s, %s);' % (ea, v)

    # Effective addresses --------------------------------------------------

    def ea(self, i):
        """Emit code computing the effective address; returns its expression."""
        if i.mode == 'dir':
            return '(uint16_t)(C.DP << 8 | 0x%02X)' % i.value
        if i.mode == 'ext':
            return i.value
        assert i.mode == 'idx'
        d = i.idx
        r = 'C.%s.w' % d['reg']
        k = d['kind']
        if k == 'off':
            e = '(uint16_t)(%s + %d)' % (r, d['off']) if d['off'] else r
        elif k == 'inc1':
            self.emit('ea = %s; %s = (uint16_t)(%s + 1);' % (r, r, r))
            e = 'ea'
        elif k == 'inc2':
            self.emit('ea = %s; %s = (uint16_t)(%s + 2);' % (r, r, r))
            e = 'ea'
        elif k == 'dec1':
            self.emit('%s = (uint16_t)(%s - 1); ea = %s;' % (r, r, r))
            e = 'ea'
        elif k == 'dec2':
            self.emit('%s = (uint16_t)(%s - 2); ea = %s;' % (r, r, r))
            e = 'ea'
        elif k == 'zero':
            e = r
        elif k == 'accA':
            e = '(uint16_t)(%s + (int8_t)C.A)' % r
        elif k == 'accB':
            e = '(uint16_t)(%s + (int8_t)C.B)' % r
        elif k == 'accD':
            e = '(uint16_t)(%s + C.D.w)' % r
        elif k == 'pc8':
            e = (i.next + d['off']) & 0xFFFF
        elif k == 'pc16':
            e = (i.next + d['off']) & 0xFFFF
        elif k == 'abs':
            e = d['off']
        if d['ind']:
            if isinstance(e, int):
                self.emit('ea = %s;' % self.rd16(e))
            else:
                if e != 'ea':
                    self.emit('ea = %s;' % e)
                self.emit('ea = RD16(ea);')
            return 'ea'
        if not isinstance(e, int) and e not in ('ea',):
            self.emit('ea = %s;' % e)
            return 'ea'
        return e

    def operand8(self, i):
        if i.mode == 'imm8':
            return '0x%02X' % i.value
        return self.rd8(self.ea(i))

    def operand16(self, i):
        if i.mode == 'imm16':
            return '0x%04X' % i.value
        ea = self.ea(i)
        if not isinstance(ea, int):
            return 'RD16(%s)' % ea
        return self.rd16(ea)

    # Control flow -----------------------------------------------------------

    def goto(self, ctx, target):
        """Return statement transferring control to a static target."""
        target &= 0xFFFF
        r = region(target)
        if r == 'F' and ('F', target) in self.leaders and ('F', target) in self.insns:
            return 'return (void *)%s;' % fname('F', target)
        if r == 'B' and ctx != 'F' and (ctx, target) in self.leaders and (ctx, target) in self.insns:
            return 'return (void *)%s;' % fname(ctx, target)
        return 'return rc_dispatch(0x%04X);' % target

    # Instructions -----------------------------------------------------------

    def insn(self, ctx, i):
        mn = i.mn
        e = self.emit
        e('/* %04X: %s */' % (i.addr, self.describe(i)))
        e('CLK(%d);' % i.cycles)

        rmw = {'NEG': 'rc_neg', 'COM': 'rc_com', 'LSR': 'rc_lsr', 'ROR': 'rc_ror', 'ASR': 'rc_asr',
               'ASL': 'rc_asl', 'ROL': 'rc_rol', 'DEC': 'rc_dec', 'INC': 'rc_inc'}
        alu = {'SUB': 'rc_sub', 'SBC': 'rc_sbc', 'AND': 'rc_and', 'EOR': 'rc_eor', 'ADC': 'rc_adc',
               'OR': 'rc_or', 'ADD': 'rc_add'}

        if mn in rmw and i.mode != 'inh':
            ea = self.ea(i)
            if isinstance(ea, int):
                e('t8 = %s;' % self.rd8(ea))
                e(self.wr8(ea, '%s(t8)' % rmw[mn]))
            else:
                e('t8 = RD(%s);' % ea)
                e('WR(%s, %s(t8));' % (ea, rmw[mn]))
            return None
        if mn[:-1] in rmw and i.mode == 'inh':
            r = mn[-1]
            e('C.%s = %s(C.%s);' % (r, rmw[mn[:-1]], r))
            return None
        if mn in ('TST', 'TSTA', 'TSTB'):
            v = 'C.' + mn[-1] if i.mode == 'inh' else self.operand8(i)
            e('rc_tst(%s);' % v)
            return None
        if mn in ('CLR', 'CLRA', 'CLRB'):
            e('C.CC_N = 0; C.CC_Z = 1; C.CC_V = 0; C.CC_C = 0;')
            if i.mode == 'inh':
                e('C.%s = 0;' % mn[-1])
            else:
                ea = self.ea(i)
                if isinstance(ea, int):
                    e(self.wr8(ea, '0'))
                else:
                    e('WR(%s, 0);' % ea)
            return None
        if mn[:-1] in alu and mn[-1] in 'AB':
            r = mn[-1]
            e('C.%s = %s(C.%s, %s);' % (r, alu[mn[:-1]], r, self.operand8(i)))
            return None
        if mn in ('CMPA', 'CMPB'):
            e('rc_cmp(C.%s, %s);' % (mn[-1], self.operand8(i)))
            return None
        if mn in ('BITA', 'BITB'):
            e('rc_tst((uint8_t)(C.%s & %s));' % (mn[-1], self.operand8(i)))
            return None
        if mn in ('LDA', 'LDB'):
            e('C.%s = %s; rc_tst(C.%s);' % (mn[-1], self.operand8(i), mn[-1]))
            return None
        if mn in ('STA', 'STB'):
            r = mn[-1]
            ea = self.ea(i)
            e(self.wr8(ea, 'C.' + r) if isinstance(ea, int) else 'WR(%s, C.%s);' % (ea, r))
            e('rc_tst(C.%s);' % r)
            return None
        reg16 = {'D': 'C.D.w', 'X': 'C.X.w', 'Y': 'C.Y.w', 'U': 'C.U.w', 'S': 'C.S.w'}
        if mn in ('LDD', 'LDX', 'LDY', 'LDU', 'LDS'):
            r = reg16[mn[2]]
            e('%s = %s; rc_tst16(%s);' % (r, self.operand16(i), r))
            if mn == 'LDS':
                e('C.NMI_Armed = 1;')
            return None
        if mn in ('STD', 'STX', 'STY', 'STU', 'STS'):
            r = reg16[mn[2]]
            ea = self.ea(i)
            e(self.wr16(ea, r) if isinstance(ea, int) else 'WR16(%s, %s);' % (ea, r))
            e('rc_tst16(%s);' % r)
            return None
        if mn in ('CMPD', 'CMPX', 'CMPY', 'CMPU', 'CMPS'):
            e('rc_cmp16(%s, %s);' % (reg16[mn[3]], self.operand16(i)))
            return None
        if mn == 'SUBD':
            e('C.D.w = rc_sub16(C.D.w, %s);' % self.operand16(i))
            return None
        if mn == 'ADDD':
            e('C.D.w = rc_add16(C.D.w, %s);' % self.operand16(i))
            return None
        if mn in ('LEAX', 'LEAY', 'LEAS', 'LEAU'):
            ea = self.ea(i)
            r = reg16[mn[3]]
            e('%s = %s;' % (r, '0x%04X' % ea if isinstance(ea, int) else ea))
            if mn in ('LEAX', 'LEAY'):
                e('C.CC_Z = (%s == 0);' % r)
            if mn == 'LEAS':
                e('C.NMI_Armed = 1;')
            return None
        if mn == 'NOP':
            return None
        if mn == 'DAA':
            e('rc_daa();')
            return None
        if mn == 'SEX':
            e('C.A = (C.B & 0x80) ? 0xFF : 0x00; C.CC_N = (C.D.w >> 15) & 1; C.CC_Z = (C.D.w == 0);')
            return None
        if mn == 'ABX':
            e('C.X.w = (uint16_t)(C.X.w + C.B);')
            return None
        if mn == 'MUL':
            e('C.D.w = (uint16_t)(C.A * C.B); C.CC_Z = (C.D.w == 0); C.CC_C = (C.B >> 7) & 1;')
            return None
        if mn == 'ORCC':
            e('rc_set_cc((uint8_t)(rc_get_cc() | 0x%02X));' % i.value)
            return 'exit'
        if mn == 'ANDCC':
            e('rc_set_cc((uint8_t)(rc_get_cc() & 0x%02X));' % i.value)
            return 'exit'
        if mn in ('TFR', 'EXG'):
            return self.tfr(ctx, i)
        if mn in ('PSHS', 'PSHU'):
            sp = 'C.S.w' if mn == 'PSHS' else 'C.U.w'
            other = 'C.U.w' if mn == 'PSHS' else 'C.S.w'
            v = i.value
            if v & 0x80:
                e('PUSH16(%s, 0x%04X);' % (sp, i.next))
            if v & 0x40:
                e('PUSH16(%s, %s);' % (sp, other))
            if v & 0x20:
                e('PUSH16(%s, C.Y.w);' % sp)
            if v & 0x10:
                e('PUSH16(%s, C.X.w);' % sp)
            if v & 0x08:
                e('PUSH8(%s, C.DP);' % sp)
            if v & 0x04:
                e('PUSH8(%s, C.B);' % sp)
            if v & 0x02:
                e('PUSH8(%s, C.A);' % sp)
            if v & 0x01:
                e('PUSH8(%s, rc_get_cc());' % sp)
            return None
        if mn in ('PULS', 'PULU'):
            sp = 'C.S.w' if mn == 'PULS' else 'C.U.w'
            other = 'C.U.w' if mn == 'PULS' else 'C.S.w'
            v = i.value
            if v & 0x01:
                e('rc_set_cc(PULL8(%s));' % sp)
            if v & 0x02:
                e('C.A = PULL8(%s);' % sp)
            if v & 0x04:
                e('C.B = PULL8(%s);' % sp)
            if v & 0x08:
                e('C.DP = PULL8(%s);' % sp)
            if v & 0x10:
                e('C.X.w = PULL16(%s);' % sp)
            if v & 0x20:
                e('C.Y.w = PULL16(%s);' % sp)
            if v & 0x40:
                e('%s = PULL16(%s);' % (other, sp))
                if mn == 'PULU':
                    e('C.NMI_Armed = 1;')
            if v & 0x80:
                e('t16 = PULL16(%s);' % sp)
                e('return rc_dispatch(t16);')
                return 'done'
            if v & 0x01:
                return 'exit'
            return None
        if mn == 'RTS':
            e('t16 = PULL16(C.S.w);')
            e('return rc_dispatch(t16);')
            return 'done'
        if mn in ('JSR', 'BSR', 'LBSR'):
            if mn == 'JSR' and i.mode != 'ext':
                ea = self.ea(i)
                e('t16 = %s;' % ('0x%04X' % ea if isinstance(ea, int) else ea))
                e('PUSH16(C.S.w, 0x%04X);' % i.next)
                e('return rc_dispatch(t16);')
                return 'done'
            e('PUSH16(C.S.w, 0x%04X);' % i.next)
            e(self.goto(ctx, i.value))
            return 'done'
        if mn == 'JMP':
            if i.mode == 'ext':
                e(self.goto(ctx, i.value))
            else:
                ea = self.ea(i)
                e('return rc_dispatch(%s);' % ('0x%04X' % ea if isinstance(ea, int) else ea))
            return 'done'
        if mn in ('BRA', 'LBRA'):
            e(self.goto(ctx, i.value))
            return 'done'
        if mn in ('BRN', 'LBRN'):
            return None
        cond = {
            'BHI': '!C.CC_C && !C.CC_Z', 'BLS': 'C.CC_C || C.CC_Z', 'BCC': '!C.CC_C', 'BCS': 'C.CC_C',
            'BNE': '!C.CC_Z', 'BEQ': 'C.CC_Z', 'BVC': '!C.CC_V', 'BVS': 'C.CC_V', 'BPL': '!C.CC_N',
            'BMI': 'C.CC_N', 'BGE': 'C.CC_N == C.CC_V', 'BLT': 'C.CC_N != C.CC_V',
            'BGT': 'C.CC_N == C.CC_V && !C.CC_Z', 'BLE': 'C.CC_Z || C.CC_N != C.CC_V'}
        name = mn[1:] if mn.startswith('L') and mn != 'LDA' else mn
        if name in cond:
            if mn.startswith('L'):
                e('if (%s) { CLK(1); %s }' % (cond[name], self.goto(ctx, i.value)))
            else:
                e('if (%s) %s' % (cond[name], self.goto(ctx, i.value)))
            return 'branch'
        raise ValueError('unhandled %s' % mn)

    def tfr(self, ctx, i):
        e = self.emit
        s, d = i.value >> 4, i.value & 15

        def get(r):
            if r & 8:
                return {8: 'C.A', 9: 'C.B', 10: 'rc_get_cc()', 11: 'C.DP'}[r]
            return {0: 'C.D.w', 1: 'C.X.w', 2: 'C.Y.w', 3: 'C.U.w', 4: 'C.S.w', 5: '0x%04X' % i.next}[r]

        def put(r, v):
            if r & 8:
                return {8: 'C.A = %s;', 9: 'C.B = %s;', 10: 'rc_set_cc(%s);', 11: 'C.DP = %s;'}[r] % v
            if r == 5:
                return 't16 = %s;' % v
            return {0: 'C.D.w = %s;', 1: 'C.X.w = %s;', 2: 'C.Y.w = %s;', 3: 'C.U.w = %s;',
                    4: 'C.S.w = %s;'}[r] % v

        tmp = 't8' if s & 8 else 't16b'
        if i.mn == 'TFR':
            e(put(d, get(s)))
            if s == 4 or d == 4:
                e('C.NMI_Armed = 1;')
        else:
            e('%s = %s;' % (tmp, get(s)))
            e(put(s, get(d)))
            e(put(d, tmp))
            if d == 4:
                e('C.NMI_Armed = 1;')
        if d == 5 or (i.mn == 'EXG' and s == 5):
            e('return rc_dispatch(t16);')
            return 'done'
        if d == 10 or (i.mn == 'EXG' and s == 10):
            return 'exit'
        return None

    def describe(self, i):
        v = i.value
        if i.mode in ('imm8',):
            return '%s #$%02X' % (i.mn, v)
        if i.mode == 'imm16':
            return '%s #$%04X' % (i.mn, v)
        if i.mode == 'dir':
            return '%s <$%02X' % (i.mn, v)
        if i.mode in ('ext', 'rel8', 'rel16'):
            return '%s $%04X' % (i.mn, v)
        if i.mode == 'idx':
            return '%s idx(%02X)' % (i.mn, i.post)
        if i.mode in ('psh', 'pul', 'tfr'):
            return '%s %02X' % (i.mn, v)
        return i.mn

    # Blocks ---------------------------------------------------------------------

    def spin_block(self, ctx, addr):
        """Emit a polling loop of the form "LDA mem / Bcc addr" that waits for
        an interrupt handler to change RAM. Nothing else can change the value
        before the time slice ends, so the remaining iterations are skipped
        by advancing the clock in whole loop passes. Returns False if the
        block is not such a loop."""
        load = self.insns.get((ctx, addr))
        if not load or load.mn not in ('LDA', 'LDB', 'TST') or load.mode not in ('dir', 'ext'):
            return False
        br = self.insns.get((ctx, load.next))
        if not br or br.mn not in ('BNE', 'BEQ', 'BPL', 'BMI') or (br.value & 0xFFFF) != addr:
            return False
        if (ctx, br.next) not in self.insns:
            return False
        if load.mode == 'ext':
            if load.value >= 0x1000:
                return False
            ram = None
        else:
            ram = '(C.DP << 8 | 0x%02X) < 0x1000' % load.value
        self.insn(ctx, load)
        cond = {'BNE': '!C.CC_Z', 'BEQ': 'C.CC_Z', 'BPL': '!C.CC_N', 'BMI': 'C.CC_N'}[br.mn]
        e = self.emit
        e('/* %04X: %s (polling loop) */' % (br.addr, self.describe(br)))
        e('CLK(%d);' % br.cycles)
        e('if (%s) {' % cond)
        e('rc_spin(%d);' % (load.cycles + br.cycles) if ram is None else
          'if (%s) rc_spin(%d);' % (ram, load.cycles + br.cycles))
        e('return (void *)%s;' % fname(ctx, addr))
        e('}')
        e(self.goto(ctx, br.next))
        return True

    def block(self, ctx, addr, single=False):
        e = self.emit
        e('static void *%s(void)' % fname(ctx, addr))
        e('{')
        e('uint16_t ea, t16, t16b; uint8_t t8; (void)ea; (void)t16; (void)t16b; (void)t8;')
        e('if (OUT_OF_TIME()) { C.PC.w = 0x%04X; return 0; }' % addr)
        if not single and self.spin_block(ctx, addr):
            e('}')
            e('')
            return
        a = addr
        first = True
        while True:
            if not first and ((ctx, a) in self.leaders or a in self.exit_at) and not single:
                e(self.goto(ctx, a))
                break
            first = False
            i = self.insns.get((ctx, a))
            if i is None:
                e('C.PC.w = 0x%04X; return 0;' % a)
                break
            res = self.insn(ctx, i)
            if res == 'done':
                break
            if res == 'exit' or single:
                e('C.PC.w = 0x%04X; return 0;' % i.next)
                break
            if region(i.next) != region(a):
                e('return rc_dispatch(0x%04X);' % i.next)
                break
            a = i.next
        e('}')
        e('')


def write_program(args):
    data = open(args.image, 'rb').read()
    image = Image(data)

    def vec(a):
        return image.fixed[a - FIXED_BASE] << 8 | image.fixed[a + 1 - FIXED_BASE]

    entries = [('F', vec(v)) for v in (0xFFF2, 0xFFF4, 0xFFF6, 0xFFF8, 0xFFFA, 0xFFFC, 0xFFFE)]
    if args.hints:
        for line in open(args.hints):
            line = line.split('#')[0].strip()
            if not line:
                continue
            f = line.split()
            ctx = 'F' if f[0] == 'F' else int(f[0])
            entries.append((ctx, int(f[1], 16)))
    exit_at = {int(a, 16) for a in args.exit_at}

    insns, leaders = discover(image, entries)
    # Addresses in exit_at (the idle loop) are left to the machine loop and
    # the interpreter: no block starts there, and jumps to them return.
    leaders = {l for l in leaders if l in insns and l[1] not in exit_at}
    g = Gen(image, insns, leaders, exit_at)
    g.emit('// Generated by tools/recomp/recomp.py from the I, Robot ROMs. Do not edit or commit.')
    g.emit('#include "rc_runtime.h"')
    g.emit('')
    for ctx, a in sorted(leaders, key=lambda k: (str(k[0]), k[1])):
        g.emit('static void *%s(void);' % fname(ctx, a))
    g.emit('')
    for ctx, a in sorted(leaders, key=lambda k: (str(k[0]), k[1])):
        g.block(ctx, a)

    fixed = ['0'] * 0xA000
    banked = [['0'] * BANK_SIZE for _ in range(NUM_BANKS)]
    for ctx, a in leaders:
        if ctx == 'F':
            fixed[a - FIXED_BASE] = fname(ctx, a)
        else:
            banked[ctx][a - BANK_BASE] = fname(ctx, a)
    g.emit('const rc_block_t rc_fixed[0xA000] = {')
    for n in range(0, 0xA000, 8):
        g.emit('    ' + ', '.join(fixed[n:n + 8]) + ',')
    g.emit('};')
    g.emit('const rc_block_t rc_banked[%d][0x2000] = {' % NUM_BANKS)
    for b in range(NUM_BANKS):
        g.emit('  {')
        for n in range(0, BANK_SIZE, 8):
            g.emit('    ' + ', '.join(banked[b][n:n + 8]) + ',')
        g.emit('  },')
    g.emit('};')
    open(args.out, 'w').write('\n'.join(g.out) + '\n')
    nf = sum(1 for k in insns if k[0] == 'F')
    print('recomp: %d instructions (%d fixed, %d banked), %d blocks' %
          (len(insns), nf, len(insns) - nf, len(leaders)), file=sys.stderr)


def write_selftest(args):
    """Random single instructions at $1000, for tools/recomp/selftest.c."""
    rnd = random.Random(args.seed)
    tables = [(0, P1), (0x10, P2), (0x11, P3)]
    g = Gen(None, {}, set(), set(), fold_rom=False)
    g.emit('// Generated by tools/recomp/recomp.py --selftest.')
    g.emit('#include "rc_runtime.h"')
    cases = []
    n = 0
    while len(cases) < args.count:
        prefix, table = rnd.choice(tables)
        opc = rnd.choice(sorted(table))
        mn = table[opc][0]
        if mn in INTERPRET:
            continue
        code = ([prefix] if prefix else []) + [opc] + [rnd.randrange(256) for _ in range(4)]
        mem = lambda a, code=code: code[a - 0x1000] if 0x1000 <= a < 0x1000 + len(code) else None
        i = decode(mem, 0x1000)
        if i is None:
            continue
        i.code = code[:i.length]
        g.insns = {('T', 0x1000): i}
        g.leaders = {('T', 0x1000)}
        fn = 'T_%d' % n
        g.emit('#define B_T_1000 %s' % fn)
        g.block('T', 0x1000, single=True)
        g.emit('#undef B_T_1000')
        cases.append((fn, i.code, g.describe(i)))
        n += 1
    g.emit('const rc_selftest_t rc_selftests[] = {')
    for fn, code, desc in cases:
        g.emit('    { %s, { %s }, %d, "%s" },' % (fn, ', '.join('0x%02X' % b for b in code), len(code), desc))
    g.emit('};')
    g.emit('const int rc_selftest_count = %d;' % len(cases))
    open(args.out, 'w').write('\n'.join(g.out) + '\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('image', nargs='?')
    ap.add_argument('out')
    ap.add_argument('--hints')
    ap.add_argument('--exit-at', action='append', default=[])
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--count', type=int, default=20000)
    args = ap.parse_args()
    if args.selftest:
        write_selftest(args)
    else:
        write_program(args)


if __name__ == '__main__':
    main()
