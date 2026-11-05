#!/usr/bin/env python3
"""Write web/mods/lists/<model>-<firmware>.txt, the patch list the Patches mod reads on the Kobo.

One line each, fields separated by tabs, so the mod can read it without a JSON parser:

    kobo-patcher-list	1
    firmware	<model>	<version>	<name>
    patch	<id>	<category>	<group or ->	<flags or ->	<title>	<warning or nothing>
    edit	<file>	<offset>	<bytes before>	<bytes after>

Edits belong to the patch before them. Flags are separated by commas: "restart" for a patch that
only takes effect after Nickel restarts, set in patches.yaml, and "cached" for one that changes code
Nickel runs once and keeps the result of, which make patches finds, and "all" for one in a category
with an "All <category>" switch, set by all_switch in patches.yaml. Patches are in the page's order:
by category, then as patches.yaml lists them.
"""
import json
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "web/mods/lists"
FORMAT = 1


def field(text):
    text = str(text)
    if "\t" in text or "\n" in text:
        raise SystemExit(f"a tab or newline can't go in the patch list: {text!r}")
    return text


def list_for(firmware, patches, all_switch):
    name = f"{firmware['model']}-{firmware['firmware']}"
    edits_by_id = json.loads((ROOT / "web/patches" / f"{name}.json").read_text())
    cached = set(json.loads((ROOT / "web/patches" / f"{name}.cached.json").read_text()))
    lines = [f"kobo-patcher-list\t{FORMAT}", "\t".join(["firmware", firmware["model"], firmware["firmware"], field(firmware["name"])])]
    for patch in patches:
        edits = edits_by_id.get(patch["id"])
        if not edits:
            continue
        flags = ",".join(flag for flag, on in (("restart", patch.get("restart")), ("cached", patch["id"] in cached),
                                               ("all", patch["category"] in all_switch)) if on) or "-"
        lines.append("\t".join(["patch", field(patch["id"]), field(patch["category"].upper()), field(patch.get("group") or "-"),
                                flags, field(patch["title"]), field(patch.get("warning") or "")]))
        for edit in edits:
            lines.append("\t".join(["edit", field(edit["file"].removeprefix(".")), str(edit["at"]), edit["find"], edit["replace"]]))
    return "\n".join(lines) + "\n"


def main():
    source = yaml.safe_load((ROOT / "patches.yaml").read_text())
    order = {category: index for index, category in enumerate(source["categories"])}
    patches = sorted(source["patches"], key=lambda patch: order[patch["category"]])
    OUT.mkdir(parents=True, exist_ok=True)
    for firmware in source["firmware"]:
        path = OUT / f"{firmware['model']}-{firmware['firmware']}.txt"
        path.write_text(list_for(firmware, patches, source.get("all_switch", [])))
        print(f"wrote {path.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
