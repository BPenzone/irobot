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
