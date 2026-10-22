#!/bin/sh
# Prints the 32X build's bring-up counters after running it headless.
# Usage: tools/retro/stats32x.sh CORE.so [retro-run options]
set -e
core=$1; shift
dump=build/sdram.dump
./build/retro-run "$core" build/irobot.32x --picodrive-dump "$dump" "$@" | grep SH-2
python3 - "$dump" <<'PY'
import struct, subprocess, sys
syms = {}
for line in subprocess.check_output(['sh-elf-nm', 'build/32x/irobot.elf']).decode().splitlines():
    f = line.split()
    if len(f) == 3:
        syms[f[2]] = int(f[0], 16)
d = open(sys.argv[1], 'rb').read()
def word(name):
    a = syms[name] - 0x06000000
    return struct.unpack('>I', d[a:a + 4])[0]
frames, vblanks = word('_stat_frames'), word('_vblanks')
print('arcade frames %d in %d 32X frames: %.0f%% of full speed' % (frames, vblanks, 100.0 * frames / max(vblanks, 1)))
PY
