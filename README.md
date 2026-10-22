# I, Robot for the Sega 32X

A personal, non-commercial port of Atari's *I, Robot* (1983) to the Sega 32X.

The original 6809 game program runs on the master SH-2, and the math box and
video processor are replaced by native SH-2 code adapted from John Manfreda's
[I, Robot Project](https://github.com/manfreda-dot-org/I-Robot-Project) (GPL-3).
The slave SH-2 runs the math box's drawing commands, rasterizes the polygons
into the 32X frame buffer and plays the sound of the four POKEY chips through
the PWM.

Status: the 32X build plays the game at full speed in PicoDrive, with sound.
On real hardware, where code in cartridge ROM is much slower, it runs at
roughly 80% of full speed in play (from a timing model, below).

## ROMs

This repository contains no ROM data and no code generated from ROM data.
You need your own copy of the MAME `irobot` ROM set. Put it in `roms/` (ignored
by git); the build reads it and writes everything derived from it to `build/`
(also ignored). A built `.32x` file contains the ROM data, so keep it to yourself.

## Layout

- `src/common/` the arcade machine model shared by both builds: memory map and
  I/O (`machine.c`), math box (`mathbox.c`), rasterizer (`raster.c`), the
  POKEY sound chips (`pokey.c`)
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
`--wav FILE` records the sound.

`--reference` runs the math box microcode from the 136029-111 to -123 PROMs
and draws its display lists the way the video processor does (ported from
MAME, in `harness/reference.c`) instead of using the port's high-level math
box. It is slow, but exact, so screenshots from the two modes show where the
high-level version differs.

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
    --frames 900 --snap 300,600,900 --press 400:start,450:a
```

Screenshots land in `build/snaps32x/`, and `--wav FILE` records the sound. On the pad, Start inserts a coin, A
starts a one-player game (and turns the view down in play), C starts a two-player game (view up), B
fires, and the D-pad steers.

### 6809 recompiler

The 32X can't interpret the 6809 fast enough, so the build translates the
program ROM into C ahead of time. `tools/recomp/recomp.py` reads the ROM image,
follows the code from the reset and interrupt vectors, and writes one C
function per basic block (`build/32x/recomp.c`, which is generated from your
ROMs and never committed). Anything it can't reach statically falls back to the
interpreter at run time.

Code that is only reached through computed jumps is listed in
`tools/recomp/hints.txt` (addresses only, no ROM data). To refresh it, run the
harness with `--recomp --hints FILE` across a few play sessions and merge the
addresses it prints. `make recomp-selftest` checks every generated instruction
against the interpreter with random registers and memory.

The generated code computes a condition code flag only where some later
instruction may read it, following the program's control flow, and brings the
6809 clock up to date only before code that can observe it. The most-run
blocks, listed in `tools/recomp/hot.txt` (addresses only), go in the 32X's
SDRAM, which is much faster than cartridge ROM for code. To refresh the list,
profile a play session with the timing model below and run
`tools/recomp/hotlist.py ELF PROFILE 120 > tools/recomp/hot.txt`; the linker
stops with "SDRAM is full" if it no longer fits.

`tools/retro/stats32x.sh` takes the same arguments as `retro-run` and reports
how many arcade frames ran per 32X frame. PicoDrive doesn't model ROM wait
states, so real hardware will be slower than it reports.
`tools/retro/picodrive-sh2-timing.patch` adds a profile and a rough
real-hardware timing model (SH-2 caches, cartridge ROM and SDRAM wait states)
to PicoDrive's SH-2 interpreter; run the patched core with `RETRO_DRC=disabled
SH2_TIMING=1`. Its estimate matched real hardware within a few percent.
`tools/retro/prof32x.py` breaks the SH-2 time down by function from its
profile. `make 32x BENCH=1` builds a benchmark variant that ignores the pad
and plays a fixed session timed in arcade frames, so that builds of different
speeds play exactly the same game and can be compared.

## License

GPL-3.0-or-later, see `LICENSE`. The 6809 core in `third_party/mc6809/` is
LGPL-3.0, see its own `LICENSE.md`. The 32X startup code in
`third_party/marsdev/` is MIT licensed, see its `LICENSE`. `tools/retro/libretro.h`
is MIT licensed by the RetroArch team.
