#!/usr/bin/env python3
"""Write web/mods/lists/2/<model>-<firmware>.txt, the patch list Kobo Patcher reads on the Kobo,
and web/mods/lists/<model>-<firmware>.txt in format 1 for the ones that can't read format 2.

One line each, fields separated by tabs, so the mod can read it without a JSON parser:

    kobo-patcher-list	2
    firmware	<model>	<version>	<name>
    patch	<id>	<category>	<group or ->	<flags or ->	<title>	<warning or nothing>
    edit	<file>	<offset>	<bytes before>	<bytes after>
    setting	<Section/name>	<value>	<value when it's turned off, or ->

Edits and settings belong to the patch before them. Format 1 has no setting lines, and leaves out
the patches with settings: a Kobo Patcher that reads only format 1 refuses a list with a line it
doesn't know. Flags are separated by commas: "restart" for a patch that
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


def field(text):
    text = str(text)
    if "\t" in text or "\n" in text:
        raise SystemExit(f"a tab or newline can't go in the patch list: {text!r}")
    return text


def list_for(firmware, patches, all_switch, version):
    name = f"{firmware['model']}-{firmware['firmware']}"
    edits_by_id = json.loads((ROOT / "web/patches" / f"{name}.json").read_text())
    cached = set(json.loads((ROOT / "web/patches" / f"{name}.cached.json").read_text()))
    lines = [f"kobo-patcher-list\t{version}", "\t".join(["firmware", firmware["model"], firmware["firmware"], field(firmware["name"])])]
    for patch in patches:
        edits = edits_by_id.get(patch["id"])
        if not edits or (version == 1 and patch.get("settings")):
            continue
        flags = ",".join(flag for flag, on in (("restart", patch.get("restart")), ("cached", patch["id"] in cached),
                                               ("all", patch["category"] in all_switch)) if on) or "-"
        lines.append("\t".join(["patch", field(patch["id"]), field(patch["category"].upper()), field(patch.get("group") or "-"),
                                flags, field(patch["title"]), field(patch.get("warning") or "")]))
        for edit in edits:
            lines.append("\t".join(["edit", field(edit["file"].removeprefix(".")), str(edit["at"]), edit["find"], edit["replace"]]))
        for setting in patch.get("settings", []):
            lines.append("\t".join(["setting", field(setting["key"]), field(setting["value"]), field(setting.get("when_off", "-"))]))
    return "\n".join(lines) + "\n"


def main():
    source = yaml.safe_load((ROOT / "patches.yaml").read_text())
    order = {category: index for index, category in enumerate(source["categories"])}
    patches = sorted(source["patches"], key=lambda patch: order[patch["category"]])
    for version, folder in ((1, OUT), (2, OUT / "2")):
        folder.mkdir(parents=True, exist_ok=True)
        for firmware in source["firmware"]:
            path = folder / f"{firmware['model']}-{firmware['firmware']}.txt"
            path.write_text(list_for(firmware, patches, source.get("all_switch", []), version))
            print(f"wrote {path.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
