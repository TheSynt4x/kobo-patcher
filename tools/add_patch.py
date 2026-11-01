#!/usr/bin/env python3
"""Add patches written in kobopatch's format to patches.yaml, and compile them for every firmware.

    make add FILE=my-patch.yaml

Nothing is added unless it fits at least one firmware. The page only offers a patch where it fits.
"""
import contextlib
import io
import re
import struct
import sys
from pathlib import Path

import yaml

import compile_patches
from compile_patches import CACHE, FILES, REPO, Binary, Misfit, compile_patch, download, read_binaries

SPEC = REPO / "patches.yaml"
# Only kobopatch's own bookkeeping, so these don't become edits.
INFO = ("Enabled", "Description", "PatchGroup", "Category")


def slug(text):
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")


def to_entry(title, steps, categories):
    """A kobopatch patch as an entry for patches.yaml, without its file yet."""
    if not isinstance(steps, list) or not all(isinstance(step, dict) and len(step) == 1 for step in steps):
        raise Misfit("it should be a list of instructions, as in kobopatch")
    info = {key: value for step in steps for key, value in step.items() if key in INFO}
    description = " ".join(str(info.get("Description", "")).split())
    if not description:
        raise Misfit("it needs a Description, which the page shows")
    if info.get("Category") not in categories:
        raise Misfit(f"it needs a Category, one of: {', '.join(categories)}")
    entry = {"id": slug(title), "title": title, "category": info["Category"], "description": description}
    if info.get("PatchGroup"):
        entry["group"] = slug(str(info["PatchGroup"]))
    entry["steps"] = [step for step in steps if next(iter(step)) not in INFO]
    return entry


def try_on(binaries, steps):
    """Which system file it fits on this firmware, or why it doesn't."""
    reasons = []
    for name, binary in binaries.items():
        try:
            if compile_patch(binary, steps):
                return name, None
            reasons.append(f"{name}: it doesn't change anything")
        except (Misfit, struct.error, ValueError, KeyError, TypeError) as error:
            reasons.append(f"{name}: {error}")
    # A missing function only says the patch is for the other file, so show the real problem if there is one.
    useful = [reason for reason in reasons if ": no symbol " not in reason]
    return None, "; ".join(useful or reasons)


def main(path):
    spec = yaml.safe_load(SPEC.read_text())
    known = {patch["id"] for patch in spec["patches"]}
    builds = []
    for build in spec["firmware"]:
        zip_path = download(build["url"], CACHE / "firmware" / f"kobo-update-{build['firmware']}.zip")
        binaries = {name: Binary(data) for name, data in read_binaries(zip_path).items()}
        builds.append((f"{build['model']} {build['firmware']}", binaries))

    added = []
    failed = False
    for title, steps in (yaml.safe_load(Path(path).read_text()) or {}).items():
        print(title)
        try:
            entry = to_entry(title, steps, spec["categories"])
            if entry["id"] in known:
                raise Misfit(f"there's already a patch called {entry['id']}")
        except Misfit as error:
            print(f"  not added: {error}")
            failed = True
            continue
        fits = set()
        for label, binaries in builds:
            name, reason = try_on(binaries, entry["steps"])
            print(f"  ok on {label}" if name else f"  doesn't fit {label}: {reason}")
            if name:
                fits.add(name)
        if len(fits) != 1:
            print("  not added: " + ("it fits no firmware" if not fits else "it fits different system files"))
            failed = True
            continue
        entry = {**{k: v for k, v in entry.items() if k != "steps"}, "file": fits.pop(), "steps": entry["steps"]}
        known.add(entry["id"])
        added.append(entry)

    if added:
        text = yaml.safe_dump(added, sort_keys=False, allow_unicode=True, width=1000)
        indented = "".join(f"  {line}\n" for line in text.splitlines())
        SPEC.write_text(SPEC.read_text().rstrip("\n") + "\n\n" + indented)
        with contextlib.redirect_stdout(io.StringIO()):
            compile_patches.main()
        print(f"\nAdded {', '.join(entry['id'] for entry in added)}. Try it with `make serve`, then see CONTRIBUTING.md.")
    return 1 if failed else 0


if __name__ == "__main__":
    if len(sys.argv) != 2 or not Path(sys.argv[1]).is_file():
        sys.exit("Usage: make add FILE=my-patch.yaml")
    sys.exit(main(sys.argv[1]))
