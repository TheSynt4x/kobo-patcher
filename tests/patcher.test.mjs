// Run with: make test
// Holds patcher.js to known good results, on a made-up firmware and on the real ones once
// `make patches` has downloaded them. REQUIRE_FIRMWARE=1 fails instead of skipping without them.
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, openAsBlob, existsSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { homedir, tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { after, test } from "node:test";
import { fileURLToPath } from "node:url";
import { gunzipSync, inflateSync } from "node:zlib";
import {
  PATCHES, Refused, SUPPORTED, UnsupportedFirmware, inspectKobo, buildPackage, buildZip, patchDriveCheck, patchDropboxCheck, planPatch, planUndo, readFirmware, readKOReader, writeZip,
  readNickel, readNickelMenu, setConf, sha256,
} from "../web/patcher.js";
import { FIRMWARE, REGISTRY, registryFor } from "../web/supported.js";

const repo = dirname(dirname(fileURLToPath(import.meta.url)));
const work = mkdtempSync(join(tmpdir(), "patcher-test-"));
after(() => rmSync(work, { recursive: true, force: true }));
const MTIME = 1700000000;
const NICKEL = "./usr/local/Kobo/libnickel.so.1.0.0";
const firmwareZip = (version) => join(homedir(), ".cache/kobo-patcher/firmware", `kobo-update-${version}.zip`);
const REAL_FIRMWARE = firmwareZip("4.38.23852");
if (process.env.REQUIRE_FIRMWARE) for (const { firmware } of FIRMWARE) assert.ok(existsSync(firmwareZip(firmware)), `no ${firmware} zip`);

function python(code, ...args) {
  return execFileSync("python3", ["-c", `import sys\n${code}`, ...args],
    { encoding: "utf8", env: { ...process.env, PYTHONDONTWRITEBYTECODE: "1" }, maxBuffer: 1 << 28 });
}

const FAKE = `
import io, struct, tarfile, zipfile
def fake(path, start):
    strings = b"\\0other\\0" + b"_ZNK6Device19supportsGoogleDriveEv" + b"\\0"
    symbols = struct.pack("<IIIBBH", 1, 0x1101, 8, 0x12, 0, 1) + struct.pack("<IIIBBH", 7, 0x1001, 8, 0x12, 0, 1)
    section = lambda kind, offset, size, link=0: struct.pack("<10I", 0, kind, 0, 0, offset, size, link, 0, 0, 0)
    elf = bytearray(0x800)
    elf[:6] = b"\\x7fELF\\x01\\x01"
    struct.pack_into("<II", elf, 28, 0x40, 0x100)
    struct.pack_into("<HHHH", elf, 42, 32, 1, 40, 3)
    struct.pack_into("<5I", elf, 0x40, 1, 0x400, 0x1000, 0x1000, 0x400)
    elf[0x100:0x178] = section(0, 0, 0) + section(11, 0x200, len(symbols), 2) + section(3, 0x300, len(strings))
    elf[0x200:0x200 + len(symbols)] = symbols
    elf[0x300:0x300 + len(strings)] = strings
    elf[0x400:0x400 + len(start)] = start
    root = io.BytesIO()
    with tarfile.open(fileobj=root, mode="w:gz") as tar:
        other = tarfile.TarInfo("./other")
        other.size = 700
        tar.addfile(other, io.BytesIO(bytes(700)))
        info = tarfile.TarInfo("./usr/local/Kobo/libnickel.so.1.0.0")
        info.size = len(elf)
        tar.addfile(info, io.BytesIO(bytes(elf)))
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("manifest.md5sum", "x")
        z.writestr("KoboRoot.tgz", root.getvalue())
`;

function fakeFirmware(name, startHex) {
  const path = join(work, name);
  python(`${FAKE}\nfake(sys.argv[1], bytes.fromhex(sys.argv[2]))`, path, startHex);
  return path;
}

test("reads, patches and packages a made-up firmware as expected", async () => {
  const path = fakeFirmware("good.zip", "98B5012100AF");
  const nickel = await readNickel(await openAsBlob(path));
  const patched = patchDriveCheck(nickel);
  const packaged = await buildPackage([{ path: NICKEL, data: patched, mode: 0o775 }], MTIME);
  writeFileSync(join(work, "tar"), gunzipSync(packaged));
  const expected = ["42891cb5f78fa7b437370798c023ced18d89172feab687a603ce5db692b767ac",
    "bbd076692135fba12d26fba602528b0485da9e2b2ad832a39aa436695ed76b2f"];
  assert.equal(await sha256(nickel), expected[0]);
  assert.equal(await sha256(readFileSync(join(work, "tar"))), expected[1]);
});

test("the original goes back unchanged when no patch is chosen", async () => {
  const path = fakeFirmware("undo.zip", "98B5012100AF");
  const nickel = await readNickel(await openAsBlob(path));
  const packaged = await buildPackage([{ path: NICKEL, data: nickel, mode: 0o775 }], MTIME);
  writeFileSync(join(work, "undo.tgz"), packaged);
  const same = python(`
import hashlib, tarfile
with tarfile.open(sys.argv[1]) as tar:
    member, = tar.getmembers()
    print(member.name, member.mode, member.uid, member.gid, hashlib.sha256(tar.extractfile(member).read()).hexdigest())`,
  join(work, "undo.tgz"));
  assert.equal(same.trim(), `${NICKEL} ${0o775} 0 0 ${await sha256(nickel)}`);
});

test("refuses a check it doesn't recognise and files that aren't firmware", async () => {
  const changed = fakeFirmware("changed.zip", "10B500000000");
  const nickel = await readNickel(await openAsBlob(changed));
  assert.throws(() => patchDriveCheck(nickel), Refused);
  writeFileSync(join(work, "junk.zip"), "not a zip");
  await assert.rejects(readNickel(await openAsBlob(join(work, "junk.zip"))), Refused);
  const noRoot = join(work, "noroot.zip");
  python(`
import zipfile
with zipfile.ZipFile(sys.argv[1], "w") as z:
    z.writestr("a.txt", "x")`, noRoot);
  await assert.rejects(readNickel(await openAsBlob(noRoot)), Refused);
});

test("the registry only offers items on firmware it can support", () => {
  for (const item of REGISTRY) {
    for (const [model, versions] of Object.entries(item.targets)) {
      for (const version of versions) {
        const build = FIRMWARE.find((b) => b.model === model && b.firmware === version);
        assert.ok(build, `${item.id}: no firmware entry for ${model} ${version}`);
        if (item.needsFirmware) assert.ok(build.nickelSha256 && build.url, `${item.id}: no hash for ${model}`);
      }
    }
  }
  assert.ok(!registryFor("N428", "4.46.23852").some((i) => i.id === "google-drive"));
  assert.ok(registryFor("N306", "4.38.23852").some((i) => i.id === "google-drive"));
  assert.ok(!registryFor("N428", "4.46.23852").some((i) => i.id === "dropbox"));
  assert.ok(registryFor("N306", "4.38.23852").some((i) => i.id === "dropbox"));
  for (const build of FIRMWARE) {
    const edits = JSON.parse(readFileSync(join(repo, "web/patches", `${build.model}-${build.firmware}.json`), "utf8"));
    for (const item of registryFor(build.model, build.firmware)) {
      if (item.kind === "patch" && !["google-drive", "dropbox"].includes(item.id)) assert.ok(edits[item.id], `${item.id}: no edits for ${build.model}`);
    }
  }
  assert.deepEqual(registryFor("N428", "4.38.23852"), []);
  assert.deepEqual(planUndo(null, {}).files, []);
});

test("a Kobo on firmware we don't have is refused with what to ask for", async () => {
  const version = new File(["N306000000000,4.1.15,4.99.1,4.1.15,4.1.15,x"], "version");
  const root = { getDirectoryHandle: async () => ({ getFileHandle: async () => ({ getFile: async () => version }) }) };
  await assert.rejects(inspectKobo(root), (error) => {
    assert.ok(error instanceof UnsupportedFirmware);
    assert.deepEqual([error.model, error.firmware], ["N306", "4.99.1"]);
    assert.match(error.message, /Kobo Nia on 4\.99\.1/);
    return true;
  });
});

test("add-on files that aren't a known release are refused", async () => {
  await assert.rejects(readNickelMenu(new Blob(["not it"])), Refused);
  await assert.rejects(readKOReader(new Blob(["not it"])), Refused);
});

test("a package can merge several files, and only holds what was chosen", async () => {
  const path = fakeFirmware("merge.zip", "98B5012100AF");
  const nickel = await readNickel(await openAsBlob(path));
  const nickelmenu = { files: [
    { path: "./usr/local/Kobo/imageformats/libnm.so", mode: 0o755, data: new Uint8Array(600).fill(1) },
    { path: "./mnt/onboard/.adds/nm/doc", mode: 0o644, data: new Uint8Array(5).fill(2) },
  ] };
  const onlyMenu = planPatch(null, [], { nickelmenu });
  assert.deepEqual(onlyMenu.files.map((f) => f.path), nickelmenu.files.map((f) => f.path));
  assert.deepEqual(onlyMenu.settings, []);
  assert.equal(onlyMenu.record, null);
  assert.throws(() => planPatch(null, ["google-drive"]), Refused);
  assert.throws(() => planPatch(null, [], { koreader: {} }), Refused);
  assert.equal(planUndo({ libnickel: nickel }, {}).files[0].data, nickel);
  assert.deepEqual(planPatch({ libnickel: nickel }, []).record, []);

  const both = planPatch({ libnickel: nickel }, ["google-drive"], { nickelmenu });
  writeFileSync(join(work, "merged.tgz"), await buildPackage(both.files, MTIME));
  const listed = python(`
import tarfile
with tarfile.open(sys.argv[1]) as tar:
    for m in tar.getmembers():
        print(m.name, oct(m.mode), m.size, m.uid)
    assert tar.extractfile("./mnt/onboard/.adds/nm/doc").read() == bytes([2]) * 5`, join(work, "merged.tgz"));
  assert.equal(listed.trim(), `${NICKEL} 0o775 ${nickel.length} 0
./usr/local/Kobo/imageformats/libnm.so 0o755 600 0
./mnt/onboard/.adds/nm/doc 0o644 5 0`);
});

async function zipToDisk(path, entries, limit) {
  const out = [];
  let size = 0;
  await writeZip(entries, {
    write(bytes) {
      out.push({ at: size, bytes: Uint8Array.from(bytes) });
      size += bytes.length;
    },
    patch(position, bytes) {
      out.push({ at: position, bytes });
    },
  }, () => {}, limit);
  const whole = new Uint8Array(size);
  for (const { at, bytes } of out) whole.set(bytes, at);
  writeFileSync(path, whole);
}

const CHECK_ZIP = `
import zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    assert z.testzip() is None
    for i in z.infolist():
        print(i.filename, i.file_size, z.read(i).hex()[:8], i.date_time[0] >= 2020)`;

for (const [label, limit] of [["a normal zip", undefined], ["zip64 records", 10]]) {
  test(`the full backup streams ${label} that Python can read`, async () => {
    const entries = [
      { name: ".kobo/Kobo/Kobo eReader.conf", data: "hello" },
      { name: "книга.epub", data: "x".repeat(70000) },
      { name: "empty", data: "" },
    ].map(({ name, data }) => ({
      name, open: async () => new File([data], name, { lastModified: Date.UTC(2026, 5, 1) }),
    }));
    const path = join(work, "full.zip");
    await zipToDisk(path, entries, limit);
    assert.equal(python(CHECK_ZIP, path), [
      ".kobo/Kobo/Kobo eReader.conf 5 68656c6c True",
      "книга.epub 70000 78787878 True",
      "empty 0  True",
      "",
    ].join("\n"));
  });
}

test("the full backup refuses a file that changes while it's read", async () => {
  const entries = [{ name: "a", open: async () => ({
    size: 5, lastModified: 0, stream: () => new Blob(["abc"]).stream(),
  }) }];
  await assert.rejects(zipToDisk(join(work, "changed.zip"), entries), Refused);
});

test("edits that don't fit or clash are refused, and unchanged firmware stays whole", () => {
  const data = Uint8Array.of(1, 2, 3, 4);
  const firmware = { libnickel: data, nickel: Uint8Array.of(9, 9) };
  const edit = (at, find, replace) => [{ file: NICKEL, at, find, replace }];
  const plan = planPatch(firmware, ["margins"], { edits: { margins: edit(1, "02", "07") } });
  assert.deepEqual([...plan.files[0].data], [1, 7, 3, 4]);
  assert.deepEqual([...data], [1, 2, 3, 4]);
  assert.deepEqual([...plan.files[1].data], [9, 9]);
  assert.throws(() => planPatch(firmware, ["margins"], { edits: { margins: edit(1, "05", "07") } }), Refused);
  assert.throws(() => planPatch(firmware, ["margins"], { edits: {} }), Refused);
  assert.throws(() => planPatch(firmware, ["margins", "font-sizes"], {
    edits: { margins: edit(1, "02", "07"), "font-sizes": edit(1, "02", "08") },
  }), Refused);
});

for (const build of FIRMWARE) {
  const zip = firmwareZip(build.firmware);
  test(`every patch for ${build.model} ${build.firmware} applies together to the real firmware`, { skip: !existsSync(zip) }, async () => {
    const firmware = await readFirmware(await openAsBlob(zip));
    assert.equal(await sha256(firmware.libnickel), build.nickelSha256);
    assert.equal(await sha256(firmware.nickel), build.nickelBinSha256);
    const edits = JSON.parse(readFileSync(join(repo, "web/patches", `${build.model}-${build.firmware}.json`), "utf8"));
    // Only one patch from a group can be chosen, so try each in turn with the rest.
    const items = registryFor(build.model, build.firmware).filter((i) => i.kind === "patch");
    for (const extra of items.filter((i) => i.group)) {
      planPatch(firmware, [...items.filter((i) => !i.group).map((i) => i.id), extra.id], { edits });
    }
    const ids = items.filter((i) => !i.group || i.id === "no-home-footer-covers").map((i) => i.id);
    const plan = planPatch(firmware, ids, { edits });
    assert.equal(plan.files.length, 2);
    for (const file of plan.files) {
      const original = file.path === NICKEL ? firmware.libnickel : firmware.nickel;
      assert.equal(file.data.length, original.length);
      assert.notEqual(await sha256(file.data), await sha256(original));
    }
    // Compressed resources still have to decompress after being replaced.
    const nickel = plan.files[1].data;
    for (const edit of Object.values(edits).flat().filter((e) => /^78(9c|da)/.test(e.find))) {
      const text = inflateSync(nickel.subarray(edit.at, edit.at + edit.replace.length / 2), { finishFlush: 2 }).toString();
      assert.ok(text.length > 0);
    }
  });
}

test("removing an add-on needs no firmware and leaves the system files alone", () => {
  const remove = { koreader: true, nickelmenu: false };
  const plan = planPatch(null, [], { remove });
  assert.deepEqual(plan.files, []);
  assert.equal(plan.remove, remove);
  assert.equal(plan.record, null);
});

const DRIVE_CONF = "googledrive_link_account_start=https://authorize.kobo.com/{region}/{language}/linkcloudstorage/provider/google_drive";

test("setConf adds the Drive settings where they belong", () => {
  const cases = [
    ["[FeatureSettings]\nAcceptPreviewUpgrades=true\n\n[Other]\nx=1\n",
      `[FeatureSettings]\nGoogleDrive=1\nAcceptPreviewUpgrades=true\n\n[Other]\nx=1\n\n[OneStoreServices]\n${DRIVE_CONF}\nkobo_googledrive_link_account_enabled=True\n`],
    ["[FeatureSettings]\nGoogleDrive=0\n",
      `[FeatureSettings]\nGoogleDrive=1\n\n[OneStoreServices]\n${DRIVE_CONF}\nkobo_googledrive_link_account_enabled=True\n`],
    ["[Other]\nGoogleDrive=0\n",
      `[Other]\nGoogleDrive=0\n\n[FeatureSettings]\nGoogleDrive=1\n\n[OneStoreServices]\n${DRIVE_CONF}\nkobo_googledrive_link_account_enabled=True\n`],
    ["[A]\r\nx=1\r\n\r\n[FeatureSettings]\r\nz=2\r\n",
      `[A]\r\nx=1\r\n\r\n[FeatureSettings]\r\nGoogleDrive=1\r\nz=2\r\n\r\n[OneStoreServices]\r\n${DRIVE_CONF}\r\nkobo_googledrive_link_account_enabled=True\r\n`],
    ["", `\n\n[FeatureSettings]\nGoogleDrive=1\n\n[OneStoreServices]\n${DRIVE_CONF}\nkobo_googledrive_link_account_enabled=True\n`],
  ];
  for (const [text, expected] of cases) {
    let ours = text;
    for (const [section, key, value] of PATCHES.find((patch) => patch.id === "google-drive").settings) {
      ours = setConf(ours, section, key, value);
    }
    assert.equal(ours, expected);
  }
});

test("setConf keeps the line ending when it replaces a value in a CRLF file", () => {
  assert.equal(setConf("[A]\r\nx=1\r\ny=2\r\n", "A", "x", "9"), "[A]\r\nx=9\r\ny=2\r\n");
});

test("the backup zip opens and holds the same bytes", () => {
  const data = new TextEncoder().encode("hello");
  const path = join(work, "backup.zip");
  writeFileSync(path, buildZip([{ name: "Kobo eReader.conf", data }, { name: "b", data: new Uint8Array(0) }]));
  const out = python(`
import zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    assert z.testzip() is None
    print(z.namelist(), z.read("Kobo eReader.conf").decode())`, path);
  assert.equal(out.trim(), "['Kobo eReader.conf', 'b'] hello");
});

test("the real firmware is recognised and its package is the known good one", { skip: !existsSync(REAL_FIRMWARE) }, async () => {
  const nickel = await readNickel(await openAsBlob(REAL_FIRMWARE));
  assert.equal(SUPPORTED[await sha256(nickel)]?.firmware, "4.38.23852");
  const patched = patchDriveCheck(nickel);
  const packaged = await buildPackage([{ path: NICKEL, data: patched, mode: 0o775 }], MTIME);
  assert.equal(await sha256(gunzipSync(packaged)), "452fcb7f8560242bb6203981e72d168d871846dd1e8b8f42b57451917098827e");
});

test("the Dropbox patch changes only its check on the real firmware, and works alongside Drive", { skip: !existsSync(REAL_FIRMWARE) }, async () => {
  const nickel = await readNickel(await openAsBlob(REAL_FIRMWARE));
  const patched = patchDropboxCheck(nickel);
  const changed = nickel.reduce((found, byte, at) => (byte === patched[at] ? found : [...found, at]), []);
  assert.deepEqual(changed, [0x96de48, 0x96de4b, 0x96de4d]);
  assert.deepEqual([...patched.subarray(0x96de48, 0x96de4e)], [0x00, 0xb5, 0x01, 0x20, 0x00, 0xbd]);
  const both = planPatch({ libnickel: nickel, nickel: new Uint8Array(1) }, ["google-drive", "dropbox"]);
  const settings = both.settings.map(([, key]) => key);
  assert.ok(settings.includes("googledrive_link_account_start") && settings.includes("dropbox_link_account_poll"));
  assert.throws(() => patchDropboxCheck(patched), Refused);
});
