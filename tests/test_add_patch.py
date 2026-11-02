#!/usr/bin/env python3
"""Run with: make test"""
import json
import os
import sys
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

import yaml

from add_patch import to_entry
from compile_patches import CACHE, Binary, Misfit, hexbytes, read_binaries

REPO = Path(__file__).resolve().parent.parent
NIA = CACHE / "firmware/kobo-update-4.38.23852.zip"

PATCH = """
Hide the Thing, please:
  - Enabled: no
  - PatchGroup: Home layout
  - Category: Covers
  - Description: |
      Hides the thing
      on the home screen.
  - BaseAddress: "SomeView::updateButtons()"
  - ReplaceBytes:
      Offset: 0xF6
      FindH: 01 21
      ReplaceH: 00 21
"""


CATEGORIES = ["Reading", "Covers"]


class ToEntry(unittest.TestCase):
    def test_takes_what_the_page_needs_and_keeps_only_the_edits(self):
        (title, steps), = yaml.safe_load(PATCH).items()
        entry = to_entry(title, steps, CATEGORIES)
        self.assertEqual(entry["id"], "hide-the-thing-please")
        self.assertEqual(entry["category"], "Covers")
        self.assertEqual(entry["title"], "Hide the Thing, please")
        self.assertEqual(entry["description"], "Hides the thing on the home screen.")
        self.assertEqual(entry["group"], "home-layout")
        self.assertEqual([next(iter(step)) for step in entry["steps"]], ["BaseAddress", "ReplaceBytes"])
        self.assertEqual(entry["steps"][1]["ReplaceBytes"]["Offset"], 0xF6)

    def test_needs_a_description(self):
        with self.assertRaises(Misfit):
            to_entry("No words", [{"Category": "Covers"}, {"BaseAddress": "x"}], CATEGORIES)

    def test_needs_a_known_category(self):
        for steps in ([{"Description": "Words"}], [{"Description": "Words"}, {"Category": "Nonsense"}]):
            with self.assertRaises(Misfit):
                to_entry("No category", steps, CATEGORIES)

    def test_refuses_something_that_isnt_a_patch(self):
        with self.assertRaises(Misfit):
            to_entry("Not a list", "hello", CATEGORIES)


class HexBytes(unittest.TestCase):
    def test_reads_spaced_hex(self):
        self.assertEqual(hexbytes("01 21"), b"\x01\x21")

    def test_refuses_hex_yaml_read_as_a_number(self):
        with self.assertRaises(Misfit):
            hexbytes(yaml.safe_load("0121"))


@unittest.skipUnless(NIA.exists() or os.environ.get("REQUIRE_FIRMWARE"), "needs the Nia firmware from make patches")
class RunsOnce(unittest.TestCase):
    """The Nia's libnickel, checked against what was found by reading its code."""

    @classmethod
    def setUpClass(cls):
        cls.nickel = Binary(read_binaries(NIA)["libnickel.so.1.0.0"])
        cls.edits = json.loads((REPO / "web/patches/N306-4.38.23852.json").read_text())

    def runs_once(self, patch):
        return any(self.nickel.runs_once(edit["at"]) for edit in self.edits[patch])

    def test_the_line_spacing_list_is_worked_out_once(self):
        self.assertTrue(self.runs_once("line-spacing"))

    def test_a_static_elsewhere_in_the_function_doesnt_count(self):
        # smartLinkGeneralMessage sets up a static null Volume, after the edit.
        self.assertTrue(self.nickel.calls(self.nickel.function_at(self.edits["no-kobo-plus"][0]["at"]), "__cxa_guard_acquire"))
        self.assertFalse(self.runs_once("no-kobo-plus"))

    def test_code_that_runs_every_time(self):
        self.assertFalse(self.runs_once("no-discover"))
        self.assertFalse(self.runs_once("font-sizes"))


unittest.main()
