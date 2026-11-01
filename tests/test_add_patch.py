#!/usr/bin/env python3
"""Run with: make test"""
import sys
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

import yaml

from add_patch import to_entry
from compile_patches import Misfit, hexbytes

PATCH = """
Hide the Thing, please:
  - Enabled: no
  - PatchGroup: Home layout
  - Description: |
      Hides the thing
      on the home screen.
  - BaseAddress: "SomeView::updateButtons()"
  - ReplaceBytes:
      Offset: 0xF6
      FindH: 01 21
      ReplaceH: 00 21
"""


class ToEntry(unittest.TestCase):
    def test_takes_what_the_page_needs_and_keeps_only_the_edits(self):
        (title, steps), = yaml.safe_load(PATCH).items()
        entry = to_entry(title, steps)
        self.assertEqual(entry["id"], "hide-the-thing-please")
        self.assertEqual(entry["title"], "Hide the Thing, please")
        self.assertEqual(entry["description"], "Hides the thing on the home screen.")
        self.assertEqual(entry["group"], "home-layout")
        self.assertEqual([next(iter(step)) for step in entry["steps"]], ["BaseAddress", "ReplaceBytes"])
        self.assertEqual(entry["steps"][1]["ReplaceBytes"]["Offset"], 0xF6)

    def test_needs_a_description(self):
        with self.assertRaises(Misfit):
            to_entry("No words", [{"Enabled": False}, {"BaseAddress": "x"}])

    def test_refuses_something_that_isnt_a_patch(self):
        with self.assertRaises(Misfit):
            to_entry("Not a list", "hello")


class HexBytes(unittest.TestCase):
    def test_reads_spaced_hex(self):
        self.assertEqual(hexbytes("01 21"), b"\x01\x21")

    def test_refuses_hex_yaml_read_as_a_number(self):
        with self.assertRaises(Misfit):
            hexbytes(yaml.safe_load("0121"))


unittest.main()
