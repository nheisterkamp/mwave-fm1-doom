#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""tools/fm1_doom.py upload against the firmware's own upload code (firmware/fm1/dpk_proto.c) on a simulated
flash (tests/dpk_sim.c), through a fake MIDI port.

  python3 tests/test_upload.py        (builds build/dpk_sim and build/test_upload.dpk first)"""
import queue
import struct
import subprocess
import sys
import threading
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import fm1_doom  # noqa: E402
import fm1_install  # noqa: E402
import wadpack  # noqa: E402

SIM = ROOT / "build" / "dpk_sim"
DPK = ROOT / "build" / "test_upload.dpk"
fm1_install.DELAY.update({"open": 0, "hs": 0.5, "retry": 0.05})


class SimLink:
    """a MIDI link to the simulator process"""

    def __init__(self, proc):
        self.proc, self.q, self.sent = proc, queue.Queue(), 0
        threading.Thread(target=self._rx, daemon=True).start()

    def _rx(self):
        while True:
            h = self.proc.stdout.read(2)
            if len(h) < 2:
                return
            n = h[0] | h[1] << 8
            self.q.put(self.proc.stdout.read(n))

    lost = False

    def send(self, pkt):
        self.proc.stdin.write(struct.pack("<H", len(pkt)) + pkt)
        self.proc.stdin.flush()
        self.sent += 1
        return True

    def read(self, timeout):
        try:
            return self.q.get(timeout=max(timeout, 0))
        except queue.Empty:
            return None

    def drain(self):
        while not self.q.empty():
            self.q.get_nowait()

    def close(self):
        pass


class SimBackend:
    def __init__(self, link):
        self.link = link

    def input_names(self):
        return ["FM-1 Doom"]

    def output_names(self):
        return ["FM-1 Doom"]

    def open(self, i, o):
        return self.link


def build():
    subprocess.run(["cc", "-O1", "-Wall", "-Wno-unused-function", f"-I{ROOT}/firmware/fm1", f"-I{ROOT}/firmware/doom",
                    "-o", str(SIM), str(ROOT / "tests" / "dpk_sim.c")], check=True)
    # a small image cut in three, as tools/build.py cuts the real one
    img, _ = wadpack.pack(ROOT / "wad" / "doom1.wad", ["E1M1"], "adpcm", 8000, [300000, 0xE0000 - 0x93000], False)
    DPK.write_bytes(img)
    return img


class Upload(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.img = build()

    def setUp(self):
        self.proc = subprocess.Popen([str(SIM), str(DPK)], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.link = SimLink(self.proc)
        self.up = fm1_install.Updater(SimBackend(self.link))

    def tearDown(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=5)

    def test_upload_then_nothing_left(self):
        fm1_doom.time.sleep = lambda s: None
        fm1_doom.upload(self.up, DPK)
        doom = fm1_doom.find(self.up)
        self.assertEqual(doom.info()["check"], 0)
        # a second upload finds the data there and writes nothing
        before = self.link.sent
        fm1_doom.upload(self.up, DPK)
        self.assertLess(self.link.sent - before, 10)

    def test_wrong_build_refused(self):
        other = bytearray(self.img)
        struct.pack_into("<I", other, 32, zlib.crc32(b"other"))
        path = ROOT / "build" / "test_upload_other.dpk"
        path.write_bytes(other)
        with self.assertRaises(fm1_doom.DoomError):
            fm1_doom.upload(self.up, path)

    def test_game_mode_refuses_writes(self):
        doom = fm1_doom.find(self.up)
        doom.reboot(False)              # (the simulator stays: a game, its data missing -> data mode)
        info = doom.info()
        self.assertEqual(info["mode"], 1)
        with self.assertRaises(fm1_doom.DoomError):
            doom.erase(0x4000)          # the app: out of range


if __name__ == "__main__":
    unittest.main()
