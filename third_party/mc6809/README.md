# MC6809 CPU core

Motorola 6809 emulator by Sean Conner, as modified by John Manfreda for the
[I, Robot Project](https://github.com/manfreda-dot-org/I-Robot-Project)
(`M6809E dll/`). Licensed under the LGPL-3.0; see `LICENSE.md`.

Changes in this copy:

- Line endings converted to LF and the header renamed to `mc6809.h`.
- Byte order is detected with `__BYTE_ORDER__`, so the core also builds for
  big-endian CPUs such as the SH-2.
- The Windows-only `_cdecl`, `__stdcall` and `__declspec` keywords compile to
  nothing on other platforms.
- `Clock` is a wrapping `uint32` so the 32X build avoids 64-bit arithmetic.
- Fixes found by checking against the 6809 datasheet: SBC's carry ignored the
  incoming carry when the subtraction wrapped; SEX cleared V; and the cycle
  counts of TFR, JMP (direct, indexed, extended), STD, and the page 2 CMPD,
  CMPY, LDS and STS instructions were short.
- The registers come first in `mc6809__t`, ahead of the memory tables, so
  that the SH-2 can reach them with short displacements from one register.
