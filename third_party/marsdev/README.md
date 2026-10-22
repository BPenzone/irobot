# marsdev 32X startup code

From the `examples/32x-skeleton` project of
[marsdev](https://github.com/andwn/marsdev) (MIT, see `LICENSE`), commit
3318d3f39823154b24ce48bfa5d8fe6e3f6cde3f. The SH-2 startup is Chilly Willy's.

- `mars_start.s` cartridge header, 68000 vector jump table, SH-2 vector
  tables and startup. Changed: header names; the master VBlank interrupt calls
  `master_vblank()` in C; the master reloads the status register address
  before releasing the slave SH-2 (the original used a register the preceding
  call had overwritten, so the slave never started).
- `md_start.s` the 68000 boot code, including Sega's standard 32X security
  block at $3F0. Changed: no font upload; `read_joypad` reads its argument at
  the offset gcc uses without `-mshort`.
- `mars.h`, `common.h` register definitions.
- `mars.ld`, `md.ld` linker scripts.
