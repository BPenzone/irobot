# I, Robot for the Sega 32X

A personal, non-commercial port of Atari's *I, Robot* (1983) to the Sega 32X.

The 6809 game program is statically recompiled from the original ROMs at build
time, and the math box and video processor are replaced by native SH-2 code
adapted from John Manfreda's [I, Robot Project](https://github.com/manfreda-dot-org/I-Robot-Project) (GPL-3).

## ROMs

This repository contains no ROM data and no code generated from ROM data.
You need your own copy of the MAME `irobot` ROM set. Put it in `roms/` (ignored
by git); the build reads it and writes generated code to `generated/` (also ignored).

## Layout

- `tools/recomp/` 6809-to-C static recompiler (runs on the host)
- `harness/` PC test harness that compares the recompiled game against a reference emulator frame by frame
- `src/sh2/` 32X master and slave SH-2 code (game logic, math box, rasterizer)
- `src/m68k/` Genesis-side 68000 code (boot, input, sound)
- `docs/` hardware notes

## PC test harness

The harness runs the arcade machine model headless on a PC and writes
screenshots, so the game logic, math box and rasterizer can be checked before
anything runs on the 32X.

```sh
cp /path/to/irobot.zip roms/
make roms          # unpacks and checks SHA-1s into roms/irobot/
make
./build/irobot-harness --frames 3000 --snap-every 300 \
    --press 400:coin:6,500:start1:6
```

Screenshots land in `build/snaps/` as PPM files. The command-line options are
listed at the top of `harness/main.c`; `--text` prints the alphanumeric
overlay, which is handy for checking game state without looking at images.

## License

GPL-3.0-or-later, see `LICENSE`. The 6809 core in `third_party/mc6809/` is
LGPL-3.0, see its own `LICENSE.md`.
