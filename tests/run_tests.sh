#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# FM-1 Doom tests on this computer (no FM-1 needed). Needs wad/doom1.wad; Docker for the 32-bit runs.
#   tests/run_tests.sh            all
#   tests/run_tests.sh quick      no 32-bit / long runs
set -e
cd "$(dirname "$0")/.."
QUICK=${1:-}
fail=0
step() { printf '\n== %s\n' "$1"; }

step "inflate (firmware/doom/inflate.c) against zlib"
python3 tests/test_inflate.py || fail=1

step "game data upload: tools/fm1_doom.py against firmware/fm1/dpk_proto.c on a simulated flash"
python3 tests/test_upload.py > build/test_upload.log 2>&1 || fail=1
tail -3 build/test_upload.log

step "music synthesizer: note pitches"
python3 -c "import sys; sys.path.insert(0,'tools'); import wadpack as w; open('build/genmidi.lmp','wb').write(w.Wad('wad/doom1.wad').named('GENMIDI'))"
cc -O1 -w -Ifirmware/doom -o build/synth_test tests/synth_test.c -lm && ./build/synth_test build/genmidi.lmp || fail=1

step "host build, the E1M1 image, a walk through E1M1"
make -s -C host >/dev/null
python3 tools/wadpack.py wad/doom1.wad build/test_e1m1.dpk | head -1
./build/host/doom -dpk build/test_e1m1.dpk -warp 1 1 -skill 3 -frames 400 -script tests/e1m1_walk.txt \
    -shot 100,400 -out build/host/test -stats 2>&1 | grep "host:\|ERROR" || fail=1

step "the shareware demos with every map (the engine's changes keep the game logic)"
python3 tools/wadpack.py wad/doom1.wad build/test_full.dpk --demos \
    --maps E1M1,E1M2,E1M3,E1M4,E1M5,E1M6,E1M7,E1M8,E1M9 | head -1
for d in demo1 demo2 demo3; do
    ./build/host/doom -dpk build/test_full.dpk -zone 8192 -playdemo $d -out build/host/test 2>&1 | grep ERROR && fail=1
done
echo "demos played"

if [ "$QUICK" != quick ]; then
    step "32-bit (as the FM-1): long random sessions in E1M1 in the firmware's zone"
    docker build -q --platform linux/386 -t fm1doom-i386 -f host/Dockerfile.i386 host >/dev/null
    # the firmware's zone (the last tools/build.py; else as measured when this was written)
    ZONE=$(python3 -c "
import re
try:
    s = open('build/fw/doom.syms').read()
    print((0x01C76000 - int(re.search(r'^([0-9a-f]+) .*\s_pool_end$', s, re.M).group(1), 16)) // 1024)
except Exception:
    print(247)")
    echo "zone $ZONE KB"
    for s in 1 2 3; do python3 tests/gen_fuzz.py 20000 $s > build/fuzz$s.txt; done
    docker run --rm --platform linux/386 -v "$PWD":/work -w /work fm1doom-i386 sh -c "
        make -s -C host OUT=../build/host32 CC=gcc OPT='-O2 -g' >/dev/null &&
        for s in 1 2 3; do ./build/host32/doom -dpk build/test_e1m1.dpk -zone $ZONE -warp 1 1 -skill 4 \
            -frames 20000 -script build/fuzz\$s.txt -stats -out build/host32/fuzz 2>&1 | grep 'host:\|ERROR'; done" \
        | tee build/fuzz.log
    grep -q ERROR build/fuzz.log && fail=1
fi

[ $fail = 0 ] && printf '\nall passed\n' || { printf '\nFAILED\n'; exit 1; }
