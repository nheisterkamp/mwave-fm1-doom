#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""A long random input script for host/doom -script: god mode (IDDQD), then random walking, turning,
strafing, shooting and using, in bursts. Stresses the zone, the renderer pools and the sound channels.

  tests/gen_fuzz.py FRAMES SEED > script.txt"""
import random
import sys

frames, seed = int(sys.argv[1]), int(sys.argv[2])
rnd = random.Random(seed)
out = []
f = 20
for c in "iddqd":                         # god mode
    out += [f"{f} down {c}", f"{f + 1} up {c}"]
    f += 3
for c in "idkfa":                         # all weapons and keys
    out += [f"{f} down {c}", f"{f + 1} up {c}"]
    f += 3
held = set()
while f < frames:
    k = rnd.choice(["up", "up", "up", "down", "left", "right", "strafel", "strafer", "fire", "use", "run", "turn",
                    "turn", "1", "2", "3", "tab"])
    dur = rnd.randint(2, 40)
    if k == "turn":
        out.append(f"{f} turn {rnd.randint(-900, 900)}")
    elif k in held:
        out.append(f"{f} up {k}")
        held.discard(k)
    else:
        out.append(f"{f} down {k}")
        if k in ("1", "2", "3", "tab", "use"):
            out.append(f"{f + 1} up {k}")
        else:
            held.add(k)
            if rnd.random() < 0.7:
                out.append(f"{f + dur} up {k}")
                held.discard(k)
    f += rnd.randint(1, 12)
print("\n".join(sorted(out, key=lambda l: int(l.split()[0]))))
