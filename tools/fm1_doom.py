#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Doom on an M-VAVE FM-1 over USB: install the firmware, upload the game data.

  tools/fm1_doom.py install [build/doom.fwsc]   the firmware (Felucca's update path; FM-1.fwsc goes back to stock)
  tools/fm1_doom.py upload [build/doom.dpk]     the game data the firmware does not hold (parts b and c)
  tools/fm1_doom.py info

The firmware holds part a of the game data (build/doom.dpk) itself; parts b and c go to the flash above it,
written by the firmware in its data mode (firmware/fm1/fm1.c dpk_service). An upload asks a running game to
restart in data mode, writes only the 4 KiB sectors that differ, checks every one, and restarts the game.
The data must come from the same build as the firmware: the firmware knows the CRCs it expects.

Needs mido and python-rtmidi (pip install mido python-rtmidi).
"""
import argparse
import struct
import sys
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fm1_install  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
DOOM_ID = "FM-1_6"                  # the identity of FM-1 Doom firmware: FM-1_6XY (tools/build.py PRODUCT)
SECTOR, PAGE = 0x1000, 256
CMD_INFO, CMD_ERASE, CMD_WRITE, CMD_CRC, CMD_REBOOT = 1, 2, 3, 4, 5
STATUS = {1: "flash error", 2: "out of range", 3: "not in data mode", 4: "unknown command"}


class DoomError(Exception):
    pass


class Doom:
    """one FM-1 running FM-1 Doom, over a MIDI link"""

    def __init__(self, dev):
        self.dev, self.link, self.seq = dev, dev.link, 0

    def request(self, cmd, payload=b"", timeout=2.0, tries=3):
        for _ in range(tries):
            self.seq = (self.seq + 1) & 0x7F
            self.link.drain()
            if not self.link.send(bytes([0xF0, 0x7D, 0x44, 0x4B, cmd, self.seq]) + fm1_install.pack7(payload) +
                                  b"\xF7"):
                raise DoomError("the FM-1 is gone")
            end = time.monotonic() + timeout
            while (left := end - time.monotonic()) > 0:
                p = self.link.read(left)
                if p is None:
                    break
                if len(p) >= 8 and p[1:4] == b"\x7D\x44\x4B" and p[4] == (cmd | 0x40) and p[5] == self.seq:
                    return p[6], fm1_install.unpack7(p[7:-1])
        raise DoomError(f"no answer to command {cmd}")

    def info(self):
        st, d = self.request(CMD_INFO)
        if st or len(d) < 48:
            raise DoomError("bad answer to INFO")
        v = struct.unpack_from("<12I", d)
        keys = ("version", "mode", "b_lo", "b_hi", "c_lo", "c_hi", "end_a", "end_b", "total", "crc_b", "crc_c", "check")
        info = dict(zip(keys, v))
        info["check"] = struct.unpack("<i", struct.pack("<I", info["check"]))[0]
        return info

    def crc(self, off, n):
        st, d = self.request(CMD_CRC, struct.pack("<II", off, n), timeout=5.0)
        if st:
            raise DoomError(f"CRC at {off:#x}: {STATUS.get(st, st)}")
        return struct.unpack("<I", d[:4])[0]

    def erase(self, off):
        st, _ = self.request(CMD_ERASE, struct.pack("<I", off), timeout=3.0)
        if st:
            raise DoomError(f"erase at {off:#x}: {STATUS.get(st, st)}")

    def write(self, off, data):
        st, _ = self.request(CMD_WRITE, struct.pack("<I", off) + data)
        if st:
            raise DoomError(f"write at {off:#x}: {STATUS.get(st, st)}")

    def reboot(self, data_mode):
        try:
            self.request(CMD_REBOOT, bytes([1 if data_mode else 0]), timeout=1.0, tries=1)
        except DoomError:
            pass                    # (it may restart before the answer leaves)
        self.link.close()


def find(up, secs=0):
    want = lambda ident: ident.text.startswith(DOOM_ID)   # noqa: E731
    dev = up.wait_for(want, secs) if secs else up.find(want)
    if not dev:
        raise DoomError("no FM-1 running FM-1 Doom found (install it first: tools/fm1_doom.py install)")
    return Doom(dev)


def upload(up, path):
    img = Path(path).read_bytes()
    magic, version, _, _, total, end_a, end_b, _, crc_b, crc_c = struct.unpack_from("<4sIIIIIIIII", img)
    if magic != b"FDPK" or total != len(img):
        raise DoomError(f"{path}: not a game data image (tools/build.py makes build/doom.dpk)")
    doom = find(up)
    info = doom.info()
    if (info["end_a"], info["end_b"], info["total"], info["crc_b"], info["crc_c"]) != (end_a, end_b, total, crc_b,
                                                                                       crc_c):
        doom.link.close()
        raise DoomError(f"{path} is not the data of the firmware on the FM-1: install the firmware of the same "
                        "build first (tools/fm1_doom.py install)")
    if info["check"] == 0:
        print("the FM-1 already has this game data")
        doom.link.close()
        return
    if info["mode"] != 1:
        print("restarting the FM-1 in data mode ...")
        doom.reboot(True)
        time.sleep(2.0)
        doom = find(up, 20)
        info = doom.info()
        if info["mode"] != 1:
            raise DoomError("the FM-1 did not come back in data mode")
    parts = [(info["b_lo"], img[end_a:end_b]), (info["c_lo"], img[end_b:])]
    if end_b - end_a > info["b_hi"] - info["b_lo"] or total - end_b > info["c_hi"] - info["c_lo"]:
        raise DoomError(f"{path} does not fit the FM-1's data areas")
    sectors = sum((len(d) + SECTOR - 1) // SECTOR for _, d in parts)
    done = written = 0
    for base, data in parts:
        for off in range(0, len(data), SECTOR):
            chunk = data[off:off + SECTOR]
            at = base + off
            for attempt in range(3):
                if doom.crc(at, len(chunk)) == zlib.crc32(chunk):
                    break
                doom.erase(at)
                for p in range(0, len(chunk), PAGE):
                    doom.write(at + p, chunk[p:p + PAGE])
                written += attempt == 0
            else:
                raise DoomError(f"the sector at {at:#x} does not verify")
            done += 1
            print(f"\r  {done}/{sectors} sectors ({written} written)", end="", flush=True)
    print()
    info = doom.info()
    if info["check"] != 0:
        raise DoomError(f"the FM-1 still finds its game data bad ({info['check']})")
    print("done: restarting the FM-1")
    doom.reboot(False)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=("install", "upload", "info"))
    ap.add_argument("file", nargs="?")
    ap.add_argument("--port", metavar="NAME", help="MIDI port to use (part of its name)")
    ap.add_argument("--yes", action="store_true", help="install without asking")
    a = ap.parse_args(argv)
    if a.command == "install":
        args = [a.file or str(ROOT / "build" / "doom.fwsc")]
        if a.port:
            args += ["--port", a.port]
        if a.yes:
            args.append("--yes")
        rc = fm1_install.main(args)
        if rc == 0:
            print("next: tools/fm1_doom.py upload (the game data; once, and after a build with other data)")
        return rc
    try:
        backend = fm1_install.MidoBackend()
    except ImportError:
        print("error: needs mido and python-rtmidi (pip install mido python-rtmidi)", file=sys.stderr)
        return 2
    up = fm1_install.Updater(backend, a.port)
    try:
        if a.command == "info":
            doom = find(up)
            i = doom.info()
            doom.link.close()
            mode = "data mode" if i["mode"] else "game"
            state = "good" if i["check"] == 0 else f"missing or bad ({i['check']})"
            print(f"{doom.dev.id.text} on {doom.dev.name}: {mode}; game data {state}; "
                  f"{i['total']} B image, {i['end_a']} B in the firmware")
            return 0
        upload(up, a.file or ROOT / "build" / "doom.dpk")
        return 0
    except (DoomError, fm1_install.InstallError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\ninterrupted: run the upload again (the FM-1 waits in data mode)", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
