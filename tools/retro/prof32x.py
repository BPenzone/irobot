#!/usr/bin/env python3
"""Summarise an SH-2 profile written by retro-run --picodrive-dump.

The profile comes from a PicoDrive libretro core with a small local change to
its SH-2 interpreter (cpu/sh2/mame/sh2pico.c, sh2_execute_interpreter): a
global `unsigned int sh2_prof[2][0x240000]` that collects the cycles each
instruction takes, indexed by CPU (0 master, 1 slave) and by address:
  ROM   0x02000000-0x023FFFFF -> (addr & 0x3FFFFF) / 2
  SDRAM 0x06000000-0x0603FFFF -> 0x200000 + (addr & 0x3FFFF) / 2
  other                       -> 0x23FFFF
Run it with RETRO_DRC=disabled so the interpreter is used, e.g.
  RETRO_DRC=disabled tools/retro/stats32x.sh path/to/core.so --frames 1800
  tools/retro/prof32x.py build/32x/irobot.elf build/sdram.dump.prof
Usage: prof32x.py ELF PROFILE [top N] [--blocks]"""
import bisect, struct, subprocess, sys
elf, prof = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[3].isdigit() else 30
syms = []
for line in subprocess.check_output(['sh-elf-nm', '-n', elf]).decode().splitlines():
    f = line.split()
    if len(f) == 3 and f[1] in 'tTwW':
        syms.append((int(f[0], 16) & 0x1fffffff, f[2]))
addrs = [a for a, _ in syms]
N = 0x240000
data = open(prof, 'rb').read()
for cpu in (0, 1):
    counts = struct.unpack_from('<%dI' % N, data, cpu * N * 4)
    per = {}
    total = 0
    for i, c in enumerate(counts):
        if not c:
            continue
        total += c
        if i == N - 1:
            name = '(other)'
        else:
            a = 0x06000000 + (i - 0x200000) * 2 if i >= 0x200000 else 0x02000000 + i * 2
            k = bisect.bisect_right(addrs, a) - 1
            name = syms[k][1] if k >= 0 else '?'
            if name.startswith('_B_') and '--blocks' not in sys.argv:
                name = '(recompiled blocks)'
        per[name] = per.get(name, 0) + c
    print('%s: %d cycles' % ('master' if cpu == 0 else 'slave', total))
    for name, c in sorted(per.items(), key=lambda x: -x[1])[:top]:
        print('  %5.1f%%  %s' % (100.0 * c / total, name))
