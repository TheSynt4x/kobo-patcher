#!/usr/bin/env python3
"""Run with: make test"""
import hashlib
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.dont_write_bytecode = True
REPO = Path(__file__).resolve().parent.parent


def pack(*args):
    return subprocess.run([sys.executable, str(REPO / "tools/pack.py"), *args], capture_output=True, text=True)


class Pack(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.so = self.dir / "libthing.so"
        self.so.write_bytes(b"\x7fELF" + bytes(60))

    def test_a_built_mod_goes_in_with_its_hash(self):
        out = self.dir / "mine.zip"
        done = pack(str(out), "--id", "mine", "--name", "My stuff", "--author", "me", "--mod", str(self.so),
                    "--targets", "N306-4.38.23852")
        self.assertEqual(done.returncode, 0, done.stderr)
        with zipfile.ZipFile(out) as z:
            self.assertEqual(sorted(z.namelist()), ["kobo-pack.txt", "mods/libthing.so"])
            lines = z.read("kobo-pack.txt").decode().splitlines()
        data = self.so.read_bytes()
        self.assertEqual(lines, ["kobo-pack\t1", "pack\tmine\t1\tMy stuff\tme",
                                 f"mod\tthing\t{hashlib.sha256(data).hexdigest()}\t{len(data)}\tN306-4.38.23852"])

    def test_names_the_page_uses_or_cant_read_are_refused(self):
        out = str(self.dir / "x.zip")
        self.assertNotEqual(pack(out, "--id", "patches", "--name", "x", "--mod", str(self.so)).returncode, 0)
        self.assertNotEqual(pack(out, "--id", "Has Spaces", "--name", "x", "--mod", str(self.so)).returncode, 0)
        self.assertNotEqual(pack(out, "--id", "empty", "--name", "x").returncode, 0)
        bad = self.dir / "mod.so"
        bad.write_bytes(b"x")
        self.assertNotEqual(pack(out, "--id", "mine", "--name", "x", "--mod", str(bad)).returncode, 0)


if __name__ == "__main__":
    unittest.main()
