#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build FM-1 Doom: the firmware, its game data and an installable package.

  tools/build.py [--iwad wad/doom1.wad] [--maps E1M1] [--release X.Y]

Outputs in build/:
  fw/doom.bin        the app (code + part a of the game data)
  loader/ota.bin     the update loader (Felucca's)
  doom.fwsc          the installable package (tools/fm1_doom.py install)
  doom.dpk           the whole game data image (tools/fm1_doom.py upload sends parts b and c)

The JieLi toolchain is Linux x86-64 only: on macOS every step runs in one linux/amd64 container
(Docker, Rosetta on Apple silicon). JIELI_TOOLCHAIN (default ~/.jieli/toolchain) and AC79_SDK
(default ~/fw-AC79_AIoT_SDK, for three files of the package) as in Felucca's BUILDING.md.
"""
import argparse
import os
import platform
import re
import shlex
import struct
import subprocess
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
FW = SRC / "firmware"
OUT = SRC / "build"
FWO = OUT / "fw"
LDR = OUT / "loader"
sys.path.insert(0, str(SRC / "tools"))
import fm1pkg_make  # noqa: E402
import lz4blk  # noqa: E402
import wadpack  # noqa: E402

APP_XIP = 0x02000120
APP_SLOT = fm1pkg_make.APP_SLOT
PART_B = 0xE0000 - 0x93000          # firmware/fm1/fm1.c DPK_B_LO..DPK_B_HI
PART_C = 0xFC000 - 0xE5000          # DPK_C_LO..DPK_C_HI
LOADER_LOAD = 0x01C0A800
LOADER_NAME = b"usb_hid_ota.bin"
DOCKER_IMAGE = os.environ.get("JIELI_DOCKER_IMAGE", "debian:bookworm-slim")
PRODUCT = "FM-1_666"           # package identity: FM-1_6XY for release X.Y (the installers want digits)
CFLAGS = ["-Os", "-ffunction-sections", "-fdata-sections", "-Wall", "-Wno-unused-function",
          "-Wno-unused-variable", "-Wno-missing-braces", "-Wno-pointer-sign", "-Wno-dangling-else",
          "-Wno-parentheses", "-Wno-tautological-compare", "-Wno-unused-but-set-variable",
          "-Wno-incompatible-pointer-types-discards-qualifiers", "-Wno-unknown-warning-option"]
LINE = re.compile(r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*\t(.*)$")


def toolchain():
    tc = Path(os.environ.get("JIELI_TOOLCHAIN", Path.home() / ".jieli" / "toolchain"))
    if not (tc / "pi32v2" / "bin" / "clang").exists():
        raise SystemExit(f"build: no JieLi toolchain in {tc} (tools/get_toolchain.sh)")
    return tc.resolve()


def native():
    return platform.system() == "Linux" and platform.machine() in ("x86_64", "AMD64")


class Runner:
    """Collects shell commands (paths relative to SRC, the toolchain at $TC) and runs them in one go:
    natively on Linux x86-64, else in one container."""

    def __init__(self):
        self.tc = toolchain()

    def run(self, script, what):
        if native():
            cmd = ["sh", "-c", script]
            env = dict(os.environ, TC=str(self.tc))
        else:
            cmd = ["docker", "run", "--rm", "--platform", "linux/amd64", "-v", f"{SRC}:/work",
                   "-v", f"{self.tc}:/opt/jieli:ro", "-w", "/work", "-e", "TC=/opt/jieli", DOCKER_IMAGE,
                   "sh", "-c", script]
            env = None
        r = subprocess.run(cmd, cwd=SRC, capture_output=True, text=True, env=env)
        if r.returncode:
            sys.stderr.write(r.stdout[-20000:] + r.stderr[-20000:])
            raise SystemExit(f"build: {what} failed")
        return r.stdout, r.stderr


def rel(p):
    return str(Path(p).resolve().relative_to(SRC))


def cc_cmd(src, obj, flags):
    return ("$TC/pi32v2/bin/clang -target pi32v2 -isystem $TC/pi32v2/include " +
            " ".join(shlex.quote(f) for f in flags) +
            f" -c {shlex.quote(rel(src))} -o {shlex.quote(rel(obj))}")


# ---- the update loader (Felucca's, unchanged) ----

def crc16(d, c=0):
    for b in d:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) if c & 0x8000 else c << 1
        c &= 0xFFFF
    return c


def ldr_head(dcrc, a, b, attr, name):
    body = struct.pack("<HIIBBH16s", dcrc, a, b, attr, 0, 0, name)
    return struct.pack("<H", crc16(body)) + body


def ldr_wrap(image):
    """ota.bin = outer JLFS head + inner head (load address) + LZ4 blocks of 4 KiB"""
    blocks = bytearray()
    for i in range(0, len(image), 4096):
        raw = image[i:i + 4096]
        c = lz4blk.compress(raw)
        if len(c) > 4096 or lz4blk.decompress(c) != raw:
            raise SystemExit(f"loader: block {i // 4096} does not compress below 4096 B")
        blocks += struct.pack("<II", len(c), len(raw)) + c
    inner = ldr_head(crc16(image), len(image), LOADER_LOAD, 0, LOADER_NAME)
    body = inner + bytes(blocks)
    return ldr_head(crc16(body), 0x20, len(body), 0x41, LOADER_NAME) + body


def build_loader(runner):
    LDR.mkdir(parents=True, exist_ok=True)
    src = FW / "loader"
    flags = ["-Os", "-ffunction-sections", "-fno-builtin", "-Wall", "-Wno-unused-function",
             "-I" + rel(FW / "hal"), "-I" + rel(FW / "fm1")]
    script = " && ".join([
        f"$TC/pi32v2/bin/clang -target pi32v2 -c {rel(src / 'crt0_ldr.S')} -o {rel(LDR / 'crt0_ldr.o')}",
        cc_cmd(src / "loader.c", LDR / "loader.o", flags),
        f"$TC/pi32v2/bin/ld --gc-sections -e _start -T {rel(src / 'loader.ld')} {rel(LDR / 'crt0_ldr.o')} "
        f"{rel(LDR / 'loader.o')} -o {rel(LDR / 'loader.elf')}",
        f"$TC/common/bin/objcopy -O binary -j .text {rel(LDR / 'loader.elf')} {rel(LDR / 'loader.bin')}",
        f"$TC/common/bin/objdump -h {rel(LDR / 'loader.elf')}"])
    hdr, _ = runner.run(script, "loader")
    for ln in hdr.splitlines():     # the loader rewrites the flash: nothing may run from XIP
        p = ln.split()
        if len(p) > 4 and p[1].startswith(".") and int(p[3], 16) >= 0x02000000 and int(p[2], 16):
            raise SystemExit(f"loader: section {p[1]} at {p[3]} is outside RAM")
    image = (LDR / "loader.bin").read_bytes()
    if not image or len(image) > 0x14000:
        raise SystemExit("loader: image empty or too big")
    ota = ldr_wrap(image)
    (LDR / "ota.bin").write_bytes(ota)
    print(f"loader   {len(image)} B, ota.bin {len(ota)} B")
    return ota


# ---- the app ----

def sources():
    doom = sorted((FW / "doom").glob("*.c"))
    fm1 = [FW / "fm1" / "fm1.c", FW / "fm1" / "lcd_conv.c", FW / "fm1" / "fm1_input_map.c",
           FW / "fm1" / "libc_ext.c"]
    asm = [FW / "crt0.S", FW / "hal" / "fm1_vec.S", FW / "hal" / "fm1_isr.S"]
    return doom, fm1, asm


def compile_all(runner, version, lcd_baud=None):
    obj = FWO / "obj"
    obj.mkdir(parents=True, exist_ok=True)
    doom, fm1, asm = sources()
    inc = ["-I" + rel(FW / "doom"), "-I" + rel(FW / "fm1"), "-I" + rel(FW / "hal"), "-I" + rel(FWO)]
    dflags = [*CFLAGS, *inc, "-DDOOM_FM1=1", "-DDOOM_FM1_TARGET=1"]
    fflags = [*CFLAGS, *inc, f'-DFELUCCA_ID="{PRODUCT}"', f'-DDOOM_FM1_VERSION="{version}"']
    if lcd_baud is not None:
        fflags.append(f"-DLCD_BAUD={lcd_baud}u")
    cmds = [cc_cmd(s, obj / (s.stem + ".o"), dflags) for s in doom]
    cmds += [cc_cmd(s, obj / (s.stem + ".o"), fflags) for s in fm1]
    cmds += [f"$TC/pi32v2/bin/clang -target pi32v2 -c {rel(s)} -o {rel(obj / (s.stem + '.o'))}" for s in asm]
    # all at once, each command's output to its own log; the logs shown, the status of each kept
    log = FWO / "log"
    log.mkdir(exist_ok=True)
    sh = ["fail=0", "pids=''"]
    for i, c in enumerate(cmds):
        sh.append(f"( {c} ) > {rel(log)}/{i}.txt 2>&1 || echo 'FAILED: {i}' >> {rel(log)}/{i}.txt &")
        sh.append("pids=\"$pids $!\"")
    sh += ["for p in $pids; do wait $p; done", f"cat {rel(log)}/*.txt", f"! grep -l FAILED {rel(log)}/*.txt"]
    (FWO / "compile.sh").write_text("\n".join(sh) + "\n")
    for f in log.glob("*.txt"):
        f.unlink()
    out, err = runner.run(f"sh {rel(FWO / 'compile.sh')}", "compile")
    warn = [ln for ln in (out + err).splitlines() if "warning:" in ln]
    if warn:
        print(f"compile  {len(warn)} warnings (first: {warn[0].strip()[:160]})")
    return [obj / (s.stem + ".o") for s in doom + fm1 + asm]


def link(runner, objs, dpk_a):
    """link with part a of the game data; returns the app image and the symbol table"""
    (FWO / "dpk_a.bin").write_bytes(dpk_a)
    lib = "$TC/pi32v2/lib"
    script = " && ".join([
        f"$TC/pi32v2/bin/clang -target pi32v2 '-DDPK_A_BIN=\"{rel(FWO / 'dpk_a.bin')}\"' "
        f"-c {rel(FW / 'fm1' / 'dpk_a.S')} -o {rel(FWO / 'dpk_a.o')}",
        f"$TC/pi32v2/bin/ld --gc-sections -T {rel(FW / 'app.ld')} -Map {rel(FWO / 'doom.map')} "
        + " ".join(rel(o) for o in objs) + f" {rel(FWO / 'dpk_a.o')} "
        f"{lib}/libc.a {lib}/libcompiler-rt.a -o {rel(FWO / 'doom.elf')}",
        f"$TC/common/bin/objcopy -O binary -j .text {rel(FWO / 'doom.elf')} {rel(FWO / 'text.bin')}",
        f"$TC/common/bin/objcopy -O binary -j .data {rel(FWO / 'doom.elf')} {rel(FWO / 'data.bin')}",
        f"$TC/common/bin/objcopy -O binary -j .ram_text {rel(FWO / 'doom.elf')} {rel(FWO / 'ramtext.bin')}",
        f"$TC/common/bin/objdump -t {rel(FWO / 'doom.elf')} > {rel(FWO / 'doom.syms')}",
        f"$TC/common/bin/objdump -d -j .ram_text {rel(FWO / 'doom.elf')} > {rel(FWO / 'ramtext.dis')}"])
    runner.run(script, "link")
    syms = (FWO / "doom.syms").read_text()

    def symv(name):
        m = re.search(r"^([0-9a-f]+) .*\s" + re.escape(name) + r"$", syms, re.M)
        if not m:
            raise SystemExit(f"link: no symbol {name}")
        return int(m.group(1), 16)
    img = bytearray((FWO / "text.bin").read_bytes())
    for sect, lname in (("ramtext.bin", "_rt_load"), ("data.bin", "_data_load")):
        load = symv(lname)
        blob = (FWO / sect).read_bytes() if (FWO / sect).exists() else b""
        if blob:
            if load % 4 or load - APP_XIP < len(img):
                raise SystemExit(f"{lname} {load:#x}: misaligned or overlaps the image")
            img += b"\xff" * (load - APP_XIP - len(img)) + blob
    img += b"\xff" * (-len(img) % 4)
    return bytes(img), syms, symv


def check(img, syms, symv):
    errors, notes = [], []
    if symv("_start") != APP_XIP:
        errors.append(f"_start is not at {APP_XIP:#x}")
    rt_calls = [ln.strip() for ln in (FWO / "ramtext.dis").read_text().splitlines()
                if LINE.match(ln) and re.search(r"\bcall\b", ln)]
    if rt_calls:
        errors.append(f".ram_text contains calls: {rt_calls[:3]}")
    if len(img) > APP_SLOT:
        errors.append(f"image {len(img)} B exceeds the app slot ({APP_SLOT} B)")
    ram = symv("_pool_end") - 0x01C08000
    zone = symv("_zone_end") - symv("_zone_start")
    notes.append(f"image {len(img)} B of {APP_SLOT}; RAM .data+.bss+.pool {ram} B, zone {zone} B")
    if zone < 160 * 1024:
        errors.append(f"zone {zone} B < 160 KiB")
    return errors, notes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iwad", type=Path, default=SRC / "wad" / "doom1.wad")
    ap.add_argument("--maps", default="E1M1")
    ap.add_argument("--sfx-rate", type=int, default=8000)
    ap.add_argument("--title", action="store_true", help="add the title picture (36 KB of flash)")
    ap.add_argument("--lcd-baud", type=int, choices=range(1, 5), metavar="1..4",
                    help="screen SPI clock = 60 MHz / (N + 1); default 4 (12 MHz, as Felucca). 2 (20 MHz) sends "
                         "a frame in 35 ms instead of 58: try it if the picture stays clean")
    ap.add_argument("--release", metavar="X.Y")
    ap.add_argument("--sdk", type=Path, help="JieLi AC79 SDK checkout (default: $AC79_SDK)")
    a = ap.parse_args()
    global PRODUCT
    version = "dev"
    name = "doom.fwsc"
    if a.release:
        m = re.fullmatch(r"(\d)\.(\d)", a.release)
        if not m:
            raise SystemExit("--release X.Y, one digit each")
        PRODUCT = "FM-1_6" + m[1] + m[2]
        version = "v" + a.release
        name = f"doom-{a.release}.fwsc"
    if not a.iwad.exists():
        raise SystemExit(f"build: {a.iwad} missing: put doom1.wad (shareware) or freedoom1.wad there (--iwad)")
    fm1pkg_make.SDK = a.sdk or os.environ.get("AC79_SDK") or Path.home() / "fw-AC79_AIoT_SDK"
    fm1pkg_make.sdk_file("uboot.boot")      # fail early without the SDK
    OUT.mkdir(exist_ok=True)
    FWO.mkdir(parents=True, exist_ok=True)
    subprocess.run([sys.executable, SRC / "tools" / "gen_font.py", FWO / "font5x7.h"], check=True)
    runner = Runner()
    ota = build_loader(runner)
    objs = compile_all(runner, version, a.lcd_baud)
    maps = [m.strip().upper() for m in a.maps.split(",") if m.strip()]
    # 1. the code alone, with an empty part a: how much of the slot is left for the data
    probe, _, _ = link(runner, objs, b"\0" * 64)
    room_a = APP_SLOT - len(probe) - 256
    # 2. the data cut to fit: part a in the rest of the slot, parts b and c in the data areas
    image, report = wadpack.pack(a.iwad, maps, "adpcm", a.sfx_rate, [room_a, PART_B], False, title=a.title)
    print("\n".join(report))
    total, end_a, end_b = struct.unpack_from("<III", image, 16)
    if total - end_b > PART_C:
        raise SystemExit(f"build: the game data is {total - end_b - PART_C} B too big for the flash: fewer maps")
    (OUT / "doom.dpk").write_bytes(image)
    img, syms, symv = link(runner, objs, image[:end_a])
    errors, notes = check(img, syms, symv)
    for n in notes:
        print("  ok   ", n)
    for e in errors:
        print("  FAIL ", e)
    if errors:
        raise SystemExit("build: checks failed")
    (FWO / "doom.bin").write_bytes(img)
    pkg = fm1pkg_make.ufw(fm1pkg_make.flash_image(img, fm1pkg_make.KEY), ota, PRODUCT)
    (OUT / name).write_bytes(pkg)
    print(f"app      {FWO / 'doom.bin'}  {len(img)} B ({len(probe)} B code, {end_a} B data)")
    print(f"data     {OUT / 'doom.dpk'}  {total} B: {end_a} in the app, {end_b - end_a} + {total - end_b} "
          f"to upload")
    print(f"package  {OUT / name}  {len(pkg)} B, identity {PRODUCT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
