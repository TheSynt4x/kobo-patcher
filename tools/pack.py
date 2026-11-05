#!/usr/bin/env python3
"""Make a pack: your own mods and patches in one zip, which anyone can add to their Kobo on the page.

    python3 tools/pack.py OUT.zip --id ID --name NAME [--author WHO] [--version V]
                          [--mod FOLDER_OR_SO]... [--patches PATCHES.YAML] [--targets MODEL-FIRMWARE,...]

--mod is a mod's folder, built here with mods/build.sh, or a .so already built. --patches is a
patches.yaml with only a patches: list, in the same format as this repo's. Its patches are compiled
for this repo's firmware, named ID.<their id>, and shown in Kobo Patcher under NAME. --targets says
which Kobos the mods are for: by default the firmware with mod: true in patches.yaml.

The zip holds:

    kobo-pack.txt                  what's in it, with each file's SHA-256 and size
    mods/lib<id>.so                each mod
    lists/<model>-<firmware>.txt   the patches for each firmware, in Kobo Patcher's list format

and kobo-pack.txt is, one line each, fields separated by tabs:

    kobo-pack	1
    pack	<id>	<version>	<name>	<author>
    mod	<id>	<sha256>	<size>	<model-firmware,...>
    list	<model-firmware>	<sha256>	<size>

Needs PyYAML, and zig for building mods.
"""
import argparse
import hashlib
import re
import subprocess
import sys
import zipfile
from pathlib import Path

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))
import write_lists  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
ID = re.compile(r"[a-z0-9][a-z0-9-]{0,31}")
# The page's own ids, which a pack can't take.
RESERVED = {"patches", "koreader"}


def field(text):
    text = str(text)
    if "\t" in text or "\n" in text:
        raise SystemExit(f"a tab or newline can't go in a pack: {text!r}")
    return text


def build_mod(path):
    """The mod's id and its .so's bytes."""
    path = Path(path).resolve()
    if path.is_file():
        name = path.name
        if not (name.startswith("lib") and name.endswith(".so")):
            raise SystemExit(f"{path} should be named lib<id>.so")
        mod_id, so = name[3:-3], path
    else:
        mod_id = path.name
        out = subprocess.run(["sh", str(ROOT / "mods/build.sh"), str(path)], check=True, capture_output=True, text=True)
        so = Path(out.stdout.strip().splitlines()[-1])
    if not ID.fullmatch(mod_id) or mod_id in RESERVED:
        raise SystemExit(f"{mod_id} can't be a mod's id in a pack")
    return mod_id, so.read_bytes()


def compile_patches(pack_id, name, path):
    """{model-firmware: list text} for the pack's patches."""
    import compile_patches as compiler
    own = yaml.safe_load((ROOT / "patches.yaml").read_text())
    theirs = yaml.safe_load(Path(path).read_text())
    patches = []
    for patch in theirs.get("patches") or []:
        if not ID.fullmatch(str(patch.get("id", ""))):
            raise SystemExit(f"{patch.get('id')!r} can't be a patch's id")
        patches.append({**patch, "id": f"{pack_id}.{patch['id']}", "category": name})
    if not patches:
        raise SystemExit(f"{path} has no patches")
    spec = {"firmware": own["firmware"], "community_patches": own["community_patches"], "categories": [name], "patches": patches}
    compiled, _, _ = compiler.compile_spec(spec)
    lists = {}
    for build in own["firmware"]:
        edits, cached = compiled[(build["model"], build["firmware"])]
        if edits:
            lists[f"{build['model']}-{build['firmware']}"] = write_lists.list_for(build, patches, [], edits, cached)
    return lists


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("out")
    parser.add_argument("--id", required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--author", default="")
    parser.add_argument("--version", default="1")
    parser.add_argument("--mod", action="append", default=[])
    parser.add_argument("--patches")
    parser.add_argument("--targets")
    args = parser.parse_args()
    if not ID.fullmatch(args.id) or args.id in RESERVED:
        raise SystemExit(f"{args.id} can't be a pack's id: lower case letters, numbers and -")
    if not args.mod and not args.patches:
        raise SystemExit("a pack needs a mod or patches")
    own = yaml.safe_load((ROOT / "patches.yaml").read_text())
    targets = args.targets or ",".join(f"{b['model']}-{b['firmware']}" for b in own["firmware"] if b.get("mod"))
    files, lines = {}, ["kobo-pack\t1", "\t".join(["pack", args.id, field(args.version), field(args.name), field(args.author)])]
    for path in args.mod:
        mod_id, data = build_mod(path)
        if f"mods/lib{mod_id}.so" in files:
            raise SystemExit(f"{mod_id} is in the pack twice")
        files[f"mods/lib{mod_id}.so"] = data
        lines.append("\t".join(["mod", mod_id, hashlib.sha256(data).hexdigest(), str(len(data)), field(targets)]))
    if args.patches:
        for target, text in compile_patches(args.id, args.name, args.patches).items():
            data = text.encode()
            files[f"lists/{target}.txt"] = data
            lines.append("\t".join(["list", target, hashlib.sha256(data).hexdigest(), str(len(data))]))
    with zipfile.ZipFile(args.out, "w", zipfile.ZIP_DEFLATED) as pack:
        pack.writestr("kobo-pack.txt", "\n".join(lines) + "\n")
        for name, data in files.items():
            pack.writestr(name, data)
    print(f"wrote {args.out}: {', '.join(files) or 'nothing'}")


if __name__ == "__main__":
    main()
