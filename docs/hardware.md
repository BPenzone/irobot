# I, Robot arcade hardware map

Compiled from John Manfreda's I, Robot Project emulator (1998 C version and
2020 C# version), which is GPL-3. Addresses are 6809 addresses. Items marked
"unverified" have not yet been checked against MAME or the schematics.

## CPU and timing

- Motorola 6809E at 1.508625 MHz (12.069 MHz crystal / 8).
- Video dot clock 5 MHz, 320 x 256 clocks per frame, so about 61 Hz refresh.
- About 24,717 CPU cycles per video frame, about 97 per scanline.
- IRQ is acknowledged by writing `$1100`; FIRQ (math box done) by writing `$1A00`.

## Memory map

| Range | Contents |
| --- | --- |
| `$0000-$07FF` | Work RAM |
| `$0800-$0FFF` | Banked work RAM, 4 banks, selected by `$1180` bits 5-6 (`_800FLIP`, `_2PLAYRAM`) |
| `$1000` | Read: coins (bit 7 right, 6 left, 5 aux), test (bit 4), coin tamper (bit 3); active low |
| `$1040` | Read: start 1 (bit 7), start 2 (bit 6), fire (bit 4); active low |
| `$1080` | Read: status, including VBLANK and math box status (bit 5) |
| `$10C0` | Read: DIP switch bank 3J |
| `$1100` | Write: IRQ acknowledge |
| `$1140` | Write: video/math control. `MATHSTART` (0 to 1 starts the math box), `ERASE` (0 to 1 clears the back buffer), `BUFSEL` (toggling flips buffers), `EXTCOMSWAP` (swaps the two COM RAM banks) |
| `$1180` | Write: bank select for `$0800` RAM and `$2000` window, plus alphanumeric palette |
| `$11C0` | Write: program ROM bank select for `$4000-$5FFF` (bits 1-3, banks 0-5) |
| `$1200-$12FF` | EEPROM (high scores and settings) |
| `$1300` | Read: unverified register |
| `$1400-$143F` | Four POKEYs; `$1420` read returns DIP switch bank 5E, `$1422` is the RANDOM register the game uses |
| `$1800-$187F` | Color RAM, 64 entries |
| `$1A00` | Write: FIRQ acknowledge |
| `$1B00` / `$1B01` | Read: joystick Y / X (ADC) |
| `$1C00-$1FFF` | Alphanumeric (text overlay) RAM |
| `$2000-$3FFF` | Banked window, selected by `$1180`: math box RAM, COM RAM 1 or 2 (display list buffers), or one of 8 pages of math box ROM (read-only) |
| `$4000-$5FFF` | Banked program ROM, 6 x 8 KB pages from 136029-405, -206, -207 |
| `$6000-$FFFF` | Fixed program ROM from 136029-208, -209, -210 |

## ROM set (MAME `irobot`)

| Files | Use |
| --- | --- |
| 136029-405, -206, -207 | Banked 6809 program, 48 KB |
| 136029-208, -209, -210 | Fixed 6809 program, 40 KB |
| 136029-101, -102, -103, -104 | Math box ROM: object geometry and playfield data, 48 KB, interleaved |
| 136029-124 | Alphanumeric character ROM |
| 136029-125 | Color PROM (32 bytes) |
| 136029-111 to -123 | Math box microcode PROMs; not needed by the high-level math box, kept as ground truth |

## Math box

Writing `MATHSTART` makes the math box read a command word from math RAM
offset 0 and run it to completion, then raise FIRQ. The high-level version
recognizes these commands:

| Command word | Operation |
| --- | --- |
| `$8400` | Render the playfield (tile grid) using the current camera |
| `$8600` | Unknown; ignored by the emulator |
| `$8800` | Roll a 3x3 matrix (pointer at word 6, sin/cos at words 7-8) |
| `$9000` | Yaw a 3x3 matrix |
| `$A000` | Pitch a 3x3 matrix |
| `$C000` | Multiply camera matrices (pointers at bytes `$F6`, `$F8`, `$FA`) |
| anything else | Render the object whose data starts at that address |

All math is signed 16-bit fixed point with 14 fractional bits. Objects are
lists of dots, vectors and flat polygons with back-face culling and 8 shade
levels from a light vector at math RAM bytes `$88-$8C`. Camera position is at
math RAM words `$12-$14`.

Math RAM is 16-bit big-endian, as is the SH-2, so the 32X port can share it
with the recompiled 6809 code without the byte swapping the PC emulators need.

## Video

- Bitmap playfield 256 x 232, double buffered, 6-bit color index into color RAM.
- Color RAM entry: 2 bits each of red, green and blue, plus 2 intensity bits
  combined with the low address bit; 64 colors on screen.
- Alphanumeric overlay: tiles from 136029-124 drawn on top of the bitmap.

## Behaviour found while bringing up the harness

- **Software watchdog.** The IRQ handler at `$6006` counts IRQs that arrive
  during VBLANK in DP `$02`. If the main loop does not reset it before it
  reaches 32, the handler jumps to `$68FE`, which restarts the game at `$6863`
  unless the main loop is idling at `$8E5E` waiting for `$07F0`.
- **POKEY randomness matters.** The object placement loop at `$76D4` reads the
  POKEY RANDOM register at `$1422` until it finds a spot far enough away. A
  generator whose successive values are just shifted copies of each other can
  keep it looping past the watchdog, which shows up as the game returning to
  attract mode right after the player presses start.
- **Buffer flip and erase share a write.** The game flips `BUFSEL` and raises
  `ERASE` in the same `$1140` write. Drawing queued before the write belongs
  to the old buffer, and the erase applies to the new one.
- **Sources disagree on two details.** MAME starts the video processor on a
  rising `EXT_START` (bit 2) and raises VBLANK at scanline 224; the 2020
  emulator treats `EXT_START` as active low and uses scanline 240. The harness
  follows the 2020 emulator for now; both work for attract mode and play.

## Notes for the port

- The 1998 emulator contains anti-tamper code (`Checksum.c`, the
  `ApplicationCorrupt` path in `IO.c`) that deliberately corrupts the game if
  the executable is modified. It must not be carried over.
- The 2020 emulator patches a few ROM bytes for cheats; the port uses the
  original program unmodified unless a patch is needed and documented.
- The 2020 emulator runs the 6809 at twice its real clock by default to reduce
  dropped frames. The port will measure both.
