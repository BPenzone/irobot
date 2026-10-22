#!/usr/bin/env python3
"""List the recompiled blocks that take the most SH-2 time, for recomp.py --hot.

Usage: hotlist.py ELF PROFILE KB > tools/recomp/hot.txt

PROFILE is an SH-2 profile from retro-run --picodrive-dump (see
tools/retro/prof32x.py), best taken with the core's real-hardware timing
model on (SH2_TIMING=1) so that time spent waiting on cartridge ROM counts.
Blocks are listed hottest first until their code in ELF adds up to KB
kilobytes. The output is 6809 addresses only, in the --hints format."""
import bisect, re, struct, subprocess, sys

elf, prof, kb = sys.argv[1], sys.argv[2], int(sys.argv[3])
syms = []
for line in subprocess.check_output(['sh-elf-nm', '-n', '-S', elf]).decode().splitlines():
    f = line.split()
    if len(f) == 4 and f[2] in 'tTwW':
        syms.append((int(f[0], 16) & 0x1fffffff, int(f[1], 16), f[3]))
addrs = [a for a, _, _ in syms]
N = 0x240000
counts = struct.unpack_from('<%dI' % N, open(prof, 'rb').read(), 0)    # master
per = {}
for i, c in enumerate(counts[:N - 1]):
    if c:
        a = 0x06000000 + (i - 0x200000) * 2 if i >= 0x200000 else 0x02000000 + i * 2
        k = bisect.bisect_right(addrs, a) - 1
        if k >= 0 and re.match(r'_B_[0-9F]_[0-9A-F]{4}$', syms[k][2]):
            per[k] = per.get(k, 0) + c
total = sum(per.values())
size = done = 0
print('# Hottest recompiled blocks, from tools/recomp/hotlist.py (%d KB)' % kb)
for k in sorted(per, key=lambda k: -per[k]):
    if size + syms[k][1] > kb * 1024:
        continue
    size += syms[k][1]
    done += per[k]
    _, ctx, addr = syms[k][2].split('_')[1:]
    print('%s %s' % (ctx, addr))
print('# %d bytes, %.1f%% of block time' % (size, 100.0 * done / max(total, 1)), file=sys.stderr)
