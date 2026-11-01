#!/usr/bin/env python3
"""Turn patches.yaml into exact byte edits for each firmware, and the list the page offers.

The page only applies the edits this writes, each checked against the bytes it expects.
A patch that doesn't fit a firmware is reported and not offered there, rather than guessed at.

Needs PyYAML and c++filt. Run it with `make patches`. It downloads the firmware and
https://github.com/pgaskin/kobopatch-patches into ~/.cache/kobo-patcher the first time.
"""
import hashlib
import json
import re
import struct
import subprocess
import tarfile
import urllib.request
import zipfile
from pathlib import Path

import yaml

ROOT = "./usr/local/Kobo/"
FILES = {"libnickel.so.1.0.0": ROOT + "libnickel.so.1.0.0", "nickel": ROOT + "nickel"}
NOP = bytes.fromhex("00BF")

REPO = Path(__file__).resolve().parent.parent
CACHE = Path.home() / ".cache/kobo-patcher"


class Misfit(Exception):
    pass


class Binary:
    def __init__(self, data):
        self.data = data
        self.symbols = self.plt = None

    def _segments(self):
        phoff = struct.unpack_from("<I", self.data, 28)[0]
        phsize, phnum = struct.unpack_from("<HH", self.data, 42)
        for i in range(phnum):
            kind, offset, vaddr, _, filesz = struct.unpack_from("<5I", self.data, phoff + i * phsize)
            if kind == 1:
                yield offset, vaddr, filesz

    def to_offset(self, address):
        for offset, vaddr, filesz in self._segments():
            if vaddr <= address < vaddr + filesz:
                return address - vaddr + offset
        raise Misfit(f"address {address:#x} isn't in the file")

    def to_address(self, at):
        for offset, vaddr, filesz in self._segments():
            if offset <= at < offset + filesz:
                return at - offset + vaddr
        raise Misfit(f"offset {at:#x} isn't loaded")

    def _load_symbols(self):
        shoff = struct.unpack_from("<I", self.data, 32)[0]
        shsize, shnum = struct.unpack_from("<HH", self.data, 46)
        sections = [struct.unpack_from("<10I", self.data, shoff + i * shsize) for i in range(shnum)]
        dynsym = next(s for s in sections if s[1] == 11)
        strings = sections[dynsym[6]]
        table = self.data[strings[4]:strings[4] + strings[5]]
        raw = []
        for at in range(dynsym[4], dynsym[4] + dynsym[5], 16):
            name_at, value = struct.unpack_from("<II", self.data, at)
            raw.append((table[name_at:table.index(b"\0", name_at)].decode(), value))
        filtered = subprocess.run(["c++filt"], input="\n".join(n for n, _ in raw), capture_output=True,
                                  text=True, check=True).stdout.splitlines()
        self.symbols = {name: value for name, (_, value) in zip(filtered, raw) if value}
        # An imported function's call goes through a stub in .plt that jumps via its GOT slot.
        names = dict(enumerate(filtered))
        strtab = sections[struct.unpack_from("<H", self.data, 50)[0]]
        shstr = self.data[strtab[4]:strtab[4] + strtab[5]]
        named = {shstr[s[0]:shstr.index(b"\0", s[0])].decode(): s for s in sections}
        self.plt = {}
        rel = named.get(".rel.plt")
        if rel:
            for at in range(rel[4], rel[4] + rel[5], 8):
                slot, info = struct.unpack_from("<II", self.data, at)
                self.plt[slot] = names[info >> 8]

    def symbol(self, name):
        if self.symbols is None:
            self._load_symbols()
        if name not in self.symbols:
            raise Misfit(f"no symbol {name}")
        return self.to_offset(self.symbols[name] & ~1)

    def plt_name(self, address):
        """The function a PLT stub at this address calls: add ip,pc,#a; add ip,ip,#b; ldr pc,[ip,#c]!"""
        if self.symbols is None:
            self._load_symbols()
        first, second, third = struct.unpack_from("<III", self.data, self.to_offset(address))
        if first & 0xFFFFF000 != 0xE28FC000 or second & 0xFFFFF000 != 0xE28CC000 or third & 0xFFFFF000 != 0xE5BCF000:
            return None
        rotated = lambda w: ((w & 0xFF) >> ((w >> 8 & 0xF) * 2) | (w & 0xFF) << (32 - (w >> 8 & 0xF) * 2)) & 0xFFFFFFFF
        return self.plt.get(address + 8 + rotated(first) + rotated(second) + (third & 0xFFF))

    def find_once(self, needle):
        first = self.data.find(needle)
        if first < 0:
            raise Misfit(f"can't find {needle[:24]!r}")
        if self.data.find(needle, first + 1) >= 0:
            raise Misfit(f"{needle[:24]!r} is there more than once")
        return first


def hexbytes(value):
    # YAML reads unspaced hex without quotes as a number, which loses its leading zeros.
    if not isinstance(value, str):
        raise Misfit(f"put quotes around the hex {value}")
    return bytes.fromhex(value.replace(" ", ""))


def blx_target(binary, at):
    """Where the 32-bit Thumb BL or BLX at this offset goes, or None if it isn't one."""
    upper, lower = struct.unpack_from("<HH", binary.data, at)
    if upper & 0xF800 != 0xF000 or lower & 0xC000 != 0xC000:
        return None
    sign = (upper >> 10) & 1
    j1, j2 = (lower >> 13) & 1, (lower >> 11) & 1
    i1, i2 = 1 - (j1 ^ sign), 1 - (j2 ^ sign)
    delta = (i1 << 23) | (i2 << 22) | ((upper & 0x3FF) << 12) | ((lower & 0x7FF) << 1)
    if sign:
        delta -= 1 << 24
    pc = binary.to_address(at) + 4
    if lower & 0x1000:  # BL stays in Thumb
        return pc + delta
    return (pc & ~3) + delta  # BLX switches to ARM, aligned


def base_address(binary, value):
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        return binary.symbol(value)
    return binary.symbol(value["Sym"]) + value.get("Rel", 0)


def patch_bytes(binary, base, spec, edits):
    at = (binary.symbol(spec["Base"]) if "Base" in spec else base) + spec.get("Offset", 0)
    if "FindInstBLX" in spec:
        wanted = spec["FindInstBLX"]["SymPLT"]
        target = blx_target(binary, at)
        if target is None or binary.plt_name(target) != wanted:
            raise Misfit(f"no call to {wanted} at {at:#x}")
        find = binary.data[at:at + 4]
    else:
        find = hexbytes(spec["FindH"])
        if binary.data[at:at + len(find)] != find:
            raise Misfit(f"expected {find.hex()} at {at:#x}, found {binary.data[at:at + len(find)].hex()}")
    if spec.get("CheckOnly"):
        return
    if spec.get("ReplaceInstNOP"):
        replace = NOP * (len(find) // 2)
    else:
        replace = hexbytes(spec["ReplaceH"])
    if len(replace) != len(find):
        raise Misfit("replacement isn't the same length")
    edits.append((at, find, replace))


def patch_value(binary, base, spec, pack, edits):
    at = base + spec.get("Offset", 0)
    find, replace = pack(spec["Find"]), pack(spec["Replace"])
    if binary.data[at:at + len(find)] != find:
        raise Misfit(f"expected {spec['Find']} at {at:#x}")
    if find != replace:
        edits.append((at, find, replace))


def patch_string(binary, base, spec, edits):
    at = base + spec.get("Offset", 0)
    find, replace = spec["Find"].encode(), spec["Replace"].encode()
    if binary.data[at:at + len(find)] != find:
        raise Misfit(f"expected {spec['Find']!r} at {at:#x}")
    if len(replace) > len(find):
        raise Misfit(f"{spec['Replace']!r} doesn't fit")
    edits.append((at, find, replace + b"\0" * (len(find) - len(replace))))


def zlib_streams(data):
    """Each Qt resource here is qCompress'd: a zlib stream, starting 78 9C or 78 DA."""
    import zlib
    for match in re.finditer(rb"\x78[\x9c\xda]", data):
        decoder = zlib.decompressobj()
        try:
            text = decoder.decompress(data[match.start():match.start() + (1 << 20)])
        except zlib.error:
            continue
        if decoder.eof:
            yield match.start(), len(data[match.start():match.start() + (1 << 20)]) - len(decoder.unused_data), text


def find_zlib(binary, needle):
    found = [s for s in zlib_streams(binary.data) if needle.encode() in s[2]]
    if len(found) != 1:
        raise Misfit(f"{len(found)} compressed resources contain {needle!r}")
    return found[0]


def patch_zlib(binary, stream, replacements, edits):
    import zlib
    at, size, text = stream
    for item in replacements:
        find, replace = item["Find"].encode(), item["Replace"].encode()
        if find not in text:
            raise Misfit(f"can't find {item['Find'][:40]!r} in a compressed resource")
        text = text.replace(find, replace)
    packed = zlib.compress(text, 9)
    if len(packed) > size:
        raise Misfit(f"recompressed resource is {len(packed) - size} bytes too big")
    edits.append((at, binary.data[at:at + size], packed + b"\0" * (size - len(packed))))


def compile_patch(binary, steps):
    edits = []
    base = 0
    stream = None
    for step in steps:
        (kind, spec), = step.items()
        if kind in ("Enabled", "Description", "PatchGroup"):
            continue
        if kind == "BaseAddress":
            base = base_address(binary, spec)
        elif kind == "FindBaseAddressHex":
            base = binary.find_once(hexbytes(spec))
        elif kind == "FindBaseAddressString":
            base = binary.find_once(spec.encode())
        elif kind == "ReplaceBytes":
            patch_bytes(binary, base, spec, edits)
        elif kind == "ReplaceInt":
            patch_value(binary, base, spec, lambda v: struct.pack("<B", v), edits)
        elif kind == "ReplaceFloat":
            patch_value(binary, base, spec, lambda v: struct.pack("<d", v), edits)
        elif kind == "ReplaceString":
            patch_string(binary, base, spec, edits)
        elif kind == "FindReplaceString":
            at = binary.find_once(spec["Find"].encode())
            patch_string(binary, at, {"Find": spec["Find"], "Replace": spec["Replace"]}, edits)
        elif kind == "FindZlib":
            stream = find_zlib(binary, spec)
        elif kind == "ReplaceZlib":
            patch_zlib(binary, stream, [spec], edits)
        elif kind == "ReplaceZlibGroup":
            patch_zlib(binary, stream, spec["Replacements"], edits)
        else:
            raise Misfit(f"this tool doesn't handle {kind}")
    return edits


def read_binaries(firmware):
    wanted = {path: name for name, path in FILES.items()}
    out = {}
    with zipfile.ZipFile(firmware) as update, update.open("KoboRoot.tgz") as root:
        with tarfile.open(fileobj=root, mode="r|gz") as tar:
            for member in tar:
                if member.name in wanted and member.isfile():
                    out[wanted[member.name]] = tar.extractfile(member).read()
    return out


def load_patches(folder, name):
    merged = {}
    for path in sorted((Path(folder) / f"{name}.yaml").glob("*.yaml")):
        merged.update(yaml.safe_load(path.read_text()) or {})
    return merged


def download(url, path):
    if path.exists():
        return path
    path.parent.mkdir(parents=True, exist_ok=True)
    print(f"downloading {url}")
    partial = path.with_name(path.name + ".part")
    # Kobo's server refuses Python's default user agent.
    request = urllib.request.Request(url, headers={"User-Agent": "kobo-patcher"})
    with urllib.request.urlopen(request) as response, partial.open("wb") as out:
        while chunk := response.read(1 << 20):
            out.write(chunk)
    partial.rename(path)
    return path


def community_folder(ref, version):
    folder = CACHE / f"kobopatch-patches-{ref}"
    if not folder.exists():
        archive = download(f"https://codeload.github.com/pgaskin/kobopatch-patches/tar.gz/{ref}", CACHE / f"{ref}.tgz")
        with tarfile.open(archive) as tar:
            tar.extractall(folder, filter="data")
        archive.unlink()
    return next(folder.iterdir()) / "src/versions" / version


def steps_for(patch, version, sources):
    if "community" in patch:
        title = patch["community"]
        if title not in sources[patch["file"]]:
            raise Misfit("not in kobopatch-patches")
        return sources[patch["file"]][title]
    if "steps" in patch:
        return patch["steps"]
    if version not in patch["per_firmware"]:
        raise Misfit("no edits written for this firmware yet")
    return patch["per_firmware"][version]


def main():
    spec = yaml.safe_load((REPO / "patches.yaml").read_text())
    for patch in spec["patches"]:
        if patch.get("category") not in spec["categories"]:
            raise SystemExit(f"{patch['id']}: category has to be one of {', '.join(spec['categories'])}")
    folder = community_folder(spec["community_patches"]["ref"], spec["community_patches"]["version"])
    sources = {name: load_patches(folder, name) for name in FILES}
    out = REPO / "web/patches"
    targets = {patch["id"]: {} for patch in spec["patches"]}
    firmware_list = []
    for build in spec["firmware"]:
        model, version = build["model"], build["firmware"]
        print(f"{model} {version}")
        zip_path = download(build["url"], CACHE / "firmware" / f"kobo-update-{version}.zip")
        raw = read_binaries(zip_path)
        binaries = {name: Binary(data) for name, data in raw.items()}
        firmware_list.append({
            **build,
            "sizeMB": round(zip_path.stat().st_size / 1e6),
            "nickelSha256": hashlib.sha256(raw["libnickel.so.1.0.0"]).hexdigest(),
            "nickelBinSha256": hashlib.sha256(raw["nickel"]).hexdigest(),
        })
        result = {}
        for patch in spec["patches"]:
            name = patch["file"]
            try:
                edits = compile_patch(binaries[name], steps_for(patch, version, sources))
                if not edits:
                    raise Misfit("it doesn't change anything")
            except (Misfit, struct.error, ValueError) as error:
                print(f"  MISFIT {patch['id']}: {error}")
                continue
            result[patch["id"]] = [
                {"file": FILES[name], "at": at, "find": find.hex(), "replace": replace.hex()}
                for at, find, replace in edits
            ]
            targets[patch["id"]].setdefault(model, []).append(version)
            print(f"  ok {patch['id']}: {len(edits)} edits")
        (out / f"{model}-{version}.json").write_text(json.dumps(result, indent=1) + "\n")
    offered = []
    for patch in spec["patches"]:
        item = {"id": patch["id"], "kind": "patch", "category": patch["category"], "title": patch["title"], "description": patch["description"]}
        item.update({key: patch[key] for key in ("warning", "group") if key in patch})
        offered.append({**item, "needsFirmware": True, "targets": targets[patch["id"]]})
    (out / "compiled.js").write_text(
        "// Written by tools/compile_patches.py from patches.yaml. Run `make patches` to change it.\n"
        f"export const FIRMWARE = {json.dumps(firmware_list, indent=2)};\n\n"
        f"export const PATCH_CATEGORIES = {json.dumps(spec['categories'], indent=2)};\n\n"
        f"export const COMPILED_PATCHES = {json.dumps(offered, indent=2)};\n"
    )


if __name__ == "__main__":
    main()
