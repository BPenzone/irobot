# I, Robot for the Sega 32X

A personal, non-commercial port of Atari's *I, Robot* (1983) to the Sega 32X.

The original 6809 game program runs on the master SH-2, and the math box and
video processor are replaced by native SH-2 code adapted from John Manfreda's
[I, Robot Project](https://github.com/manfreda-dot-org/I-Robot-Project) (GPL-3).
The slave SH-2 rasterizes the polygons into the 32X frame buffer.

Status: the 32X build boots and runs the attract mode in PicoDrive at roughly a
quarter of full speed. Speed work is next.

## ROMs

This repository contains no ROM data and no code generated from ROM data.
You need your own copy of the MAME `irobot` ROM set. Put it in `roms/` (ignored
by git); the build reads it and writes everything derived from it to `build/`
(also ignored). A built `.32x` file contains the ROM data, so keep it to yourself.

## Layout

- `src/common/` the arcade machine model shared by both builds: memory map and
  I/O (`machine.c`), math box (`mathbox.c`), rasterizer (`raster.c`)
- `src/32x/` 32X-specific code: SH-2 entry points, the video backend that
  splits work between the two SH-2s, and the 68000 side
- `harness/` PC test harness that runs the machine model headless
- `tools/` ROM unpacking, the ROM image builder, and a headless libretro runner
- `third_party/` the 6809 core and the marsdev 32X startup code
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

## 32X build

On Ubuntu 24.04 the toolchains are packaged:

```sh
sudo apt install gcc-sh-elf binutils-sh-elf libnewlib-sh-elf-dev \
    gcc-m68k-linux-gnu binutils-m68k-linux-gnu
make roms          # once, as above
make 32x           # writes build/irobot.32x
```

`build/irobot.32x` has only been tested in PicoDrive so far. For headless testing, build
PicoDrive's libretro core (`make -f Makefile.libretro` in a PicoDrive checkout)
and use the runner:

```sh
make build/retro-run
./build/retro-run path/to/picodrive_libretro.so build/irobot.32x \
    --frames 900 --snap 300,600,900 --press 400:a,450:start
```

Screenshots land in `build/snaps32x/`. On the pad, A inserts a coin, Start
starts a one-player game, B and C fire, and the D-pad steers.

## License

GPL-3.0-or-later, see `LICENSE`. The 6809 core in `third_party/mc6809/` is
LGPL-3.0, see its own `LICENSE.md`. The 32X startup code in
`third_party/marsdev/` is MIT licensed, see its `LICENSE`. `tools/retro/libretro.h`
is MIT licensed by the RetroArch team.
