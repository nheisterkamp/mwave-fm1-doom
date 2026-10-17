#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""firmware/doom/inflate.c against zlib: every lump of the IWAD, at three levels, and some edge cases."""
import random
import subprocess
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import wadpack  # noqa: E402

DRIVER = r'''
#include <stdio.h>
#include <stdlib.h>
#include "inflate.h"
int main(int c, char **v)
{
    long n, want = atol(v[1]), r;
    unsigned char *b = malloc(4 << 20), *o = malloc(want + 16);
    n = (long)fread(b, 1, 4 << 20, stdin);
    r = inflate_raw(o, want, b, n);
    if (r != want) return 1;
    fwrite(o, 1, r, stdout);
    return 0;
}
'''


def main():
    out = ROOT / "build" / "inflate_test"
    src = ROOT / "build" / "inflate_test.c"
    src.parent.mkdir(exist_ok=True)
    src.write_text(DRIVER)
    subprocess.run(["cc", "-O2", f"-I{ROOT}/firmware/doom", "-o", str(out), str(src),
                    str(ROOT / "firmware" / "doom" / "inflate.c")], check=True)
    wad = wadpack.Wad(ROOT / "wad" / "doom1.wad")
    rnd = random.Random(1)
    tests = [wad.read(i) for i in range(len(wad.lumps)) if wad.lumps[i][2]]
    tests += [bytes(rnd.getrandbits(8) for _ in range(3000)), b"a" * 100000, b"", bytes(range(256)) * 50]
    bad = n = 0
    for level in (9, 1, 0):
        for d in tests if level == 9 else tests[::5]:
            c = zlib.compressobj(level, zlib.DEFLATED, -15, 9)
            r = subprocess.run([str(out), str(len(d))], input=c.compress(d) + c.flush(), capture_output=True)
            n += 1
            if r.returncode or r.stdout != d:
                bad += 1
    print(f"inflate: {n} streams, {bad} wrong")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
