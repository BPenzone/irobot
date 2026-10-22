#!/usr/bin/env python3
"""Unpack and verify a MAME `irobot` ROM set.

Usage: extract_roms.py irobot.zip OUT_DIR

Writes 136029-*.bin into OUT_DIR after checking each file's SHA-1 against
the list used by the I, Robot Project emulator. The zip may also contain a
nested irobot.zip or an irobot/ folder.
"""

import hashlib
import io
import os
import sys
import zipfile

# SHA-1 of each ROM the port needs (from the I, Robot Project's RomSet.cs).
EXPECTED = {
    "136029-101": "868bb3fe5657a4ce45c3dd04ba26a7fb5a5ded42",
    "136029-102": "787ec3e642e1dc3417477348afa88c764e1f2a88",
    "136029-103": "c1f4041a58f395e24855254849604dfe3b8b0d71",
    "136029-104": "b9fd76eae8ca24fa3abc30c46bbf30d89943d97d",
    "136029-405": "5d71d8ec80c9be4726189d48ad519b4638160d64",
    "136029-206": "bd94ad4d536f681efa81153050a12098a31d79cf",
    "136029-207": "2e0c1e4c265e7d232ca86d5c8760e32fc49fe08d",
    "136029-208": "5b476dbee8b171a96301b2204420161333d4ca97",
    "136029-209": "a88ae0cc9ee22aa5dd3db0173f24313189f894f8",
    "136029-210": "daa77293678b7e822d0672b90789c53098c5451e",
    "136029-124": "743c6570c787bc9a2a14716adc66b8e2fe57129f",
    "136029-125": "5b42cc065bfac467028ae883844c8f94465c3666",
}


def collect(z, found):
    for info in z.infolist():
        name = os.path.basename(info.filename)
        if not name:
            continue
        data = z.read(info)
        if name.lower().endswith(".zip"):
            collect(zipfile.ZipFile(io.BytesIO(data)), found)
            continue
        stem = os.path.splitext(name)[0].replace(".", "-")
        if stem in EXPECTED:
            found[stem] = data


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    found = {}
    collect(zipfile.ZipFile(sys.argv[1]), found)
    os.makedirs(sys.argv[2], exist_ok=True)
    ok = True
    for stem, sha in EXPECTED.items():
        data = found.get(stem)
        if data is None:
            print(f"missing  {stem}")
            ok = False
            continue
        if hashlib.sha1(data).hexdigest() != sha:
            print(f"BAD SHA1 {stem}")
            ok = False
            continue
        with open(os.path.join(sys.argv[2], stem + ".bin"), "wb") as f:
            f.write(data)
        print(f"ok       {stem}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
