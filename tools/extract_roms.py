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

# Math box microcode PROMs (SHA-1 from MAME). Only the harness's --reference
# mode uses them, so they are unpacked when present but not required.
OPTIONAL = {
    "136029-111": "33dee2382e1e3899ffbaea859a67af7334270b4a",
    "136029-112": "4e1ea039e7a3e341796097b0c6943a4805b89f56",
    "136029-113": "63a3818450a76d230a75f038b140c3934659313e",
    "136029-114": "34887df0f1ac064b4cf4252a225406e8b30872c6",
    "136029-115": "46b4495002ddf80668a66a4f85cab99432677b50",
    "136029-116": "e4caab90910b3aa16c314909f8c02eaf212449a1",
    "136029-117": "39532fc1b14714396764500a9b1c9e4fed97a970",
    "136029-118": "163e8e764b400d726c725b6a45901c311e62667e",
    "136029-119": "964a87c879c953563ca84f8e3c1201302c7b2b91",
    "136029-120": "35ca23d5bbdc2827afb823a974864b96eb135797",
    "136029-121": "4628f8af43d82e578833b1452ec747eeb822b4e4",
    "136029-122": "02398ba6e7c56d961bf92e2755e530db1144219d",
    "136029-123": "85f338eeff7d8ed58804611bf8446ebb697d196d",
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
        if stem in EXPECTED or stem in OPTIONAL:
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
    for stem, sha in OPTIONAL.items():
        data = found.get(stem)
        if data is None or hashlib.sha1(data).hexdigest() != sha:
            print(f"skipped  {stem} (optional)")
            continue
        with open(os.path.join(sys.argv[2], stem + ".bin"), "wb") as f:
            f.write(data)
        print(f"ok       {stem}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
