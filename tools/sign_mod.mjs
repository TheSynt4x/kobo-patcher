#!/usr/bin/env node
// Sign a mod's update for nk.updates: write what goes at the address the mod gives nk.updates,
// manifest.txt, manifest.sig and lib<mod>.so, and the files the mod keeps up to date.
//
//   node tools/sign_mod.mjs FOLDER OUT [--key KEY.pem] [--targets MODEL-FIRMWARE,...] [--files DIR]
//   node tools/sign_mod.mjs --new-key KEY.pem
//
// FOLDER is the mod's folder, built with mods/build.sh, so its build/lib<mod>.so is beside it.
// OUT is the folder to write to. The key is KEY.pem, or the environment's KOBO_MOD_SIGNING_KEY, an
// Ed25519 key in PEM. Its public half has to be in one of the mod's scripts, as nk.updates' key, or
// nothing is signed: Kobos would refuse it. --new-key makes one, once, and prints its public half.
//
// --files DIR puts every file under DIR at the same path in OUT, with a line of its own, so a mod
// that names that path in nk.updates' files gets it saved at that path in its .adds folder.
//
// The manifest is, fields separated by tabs:
//
//   kobo-mod-manifest	1
//   mod	<sha512>	<size>	<model-firmware,...>	<version>	<release or dev>
//   file	<path>	<sha512>	<size>
//
// The version is FOLDER/version, which build.sh builds in as nk.version: a Kobo is only offered a
// higher one than its own. Release or dev is what build.sh stamped the build. The targets are the
// Kobos the mod updates itself on, where it has been tried: by default the firmware marked mod: true
// in patches.yaml.
import { createHash, createPrivateKey, createPublicKey, generateKeyPairSync, sign } from "node:crypto";
import { copyFileSync, existsSync, mkdirSync, readdirSync, readFileSync, statSync, writeFileSync } from "node:fs";
import { basename, dirname, join, relative, resolve, sep } from "node:path";
import { parseArgs } from "node:util";
import { FIRMWARE } from "../web/patches/compiled.js";

const rawPublicKey = (key) => createPublicKey(key).export({ format: "der", type: "spki" }).subarray(-32).toString("hex");
const fail = (message) => {
  console.error(message);
  process.exit(1);
};

const { values, positionals } = parseArgs({
  allowPositionals: true,
  options: { key: { type: "string" }, targets: { type: "string" }, files: { type: "string" }, "new-key": { type: "string" } },
});

if (values["new-key"]) {
  const file = values["new-key"];
  if (existsSync(file)) fail(`${file} already exists`);
  const { privateKey } = generateKeyPairSync("ed25519");
  mkdirSync(dirname(resolve(file)), { recursive: true });
  writeFileSync(file, privateKey.export({ format: "pem", type: "pkcs8" }), { mode: 0o600 });
  console.log(`wrote ${file}. Keep it secret, and give its public half to nk.updates:`);
  console.log(`key = "${rawPublicKey(privateKey)}"`);
  process.exit(0);
}

if (positionals.length !== 2) fail("usage: node tools/sign_mod.mjs FOLDER OUT [--key KEY.pem] [--targets MODEL-FIRMWARE,...]");
const folder = resolve(positionals[0]);
const out = resolve(positionals[1]);
const name = basename(folder);
const so = join(dirname(folder), "build", `lib${name}.so`);
if (!existsSync(so)) fail(`no ${so}: build it with mods/build.sh first`);

const versionFile = join(folder, "version");
if (!existsSync(versionFile)) fail(`no ${versionFile}: a mod that updates itself needs a version`);
const version = readFileSync(versionFile, "utf8").trim();
if (!/^[1-9][0-9]*$/.test(version)) fail(`${versionFile} should be a whole number from 1, not '${version}'`);

const pem = values.key ? readFileSync(values.key, "utf8") : process.env.KOBO_MOD_SIGNING_KEY;
if (!pem) fail("no signing key: give --key KEY.pem, or set KOBO_MOD_SIGNING_KEY");
const key = createPrivateKey(pem);
const publicKey = rawPublicKey(key);
const scripts = readdirSync(folder).filter((file) => file.endsWith(".lua")).map((file) => readFileSync(join(folder, file), "utf8"));
if (!scripts.some((text) => text.toLowerCase().includes(publicKey)))
  fail(`the signing key's public half, ${publicKey}, isn't in any of ${name}'s scripts, so Kobos would refuse what it signs`);

const data = readFileSync(so);
// build.sh's stamp, "nk-build: release" or "nk-build: dev ...", which the kit keeps in the .so.
const stamp = data.toString("latin1").match(/nk-build: ([^\0]+)/)?.[1];
if (!stamp) fail(`${so} has no build stamp: build it again with this kit`);
const release = stamp === "release";

const targets = values.targets ?? FIRMWARE.filter((build) => build.mod).map((build) => `${build.model}-${build.firmware}`).join(",");
if (!/^[A-Za-z0-9.-]+(,[A-Za-z0-9.-]+)*$/.test(targets)) fail(`targets should be model-firmware, separated by commas, not '${targets}'`);

// Every file under a folder, as paths relative to it with /, in order.
const walk = (root, folder = root) =>
  readdirSync(folder).sort().flatMap((entry) => {
    const path = join(folder, entry);
    return statSync(path).isDirectory() ? walk(root, path) : [relative(root, path).split(sep).join("/")];
  });
const files = values.files ? walk(resolve(values.files)) : [];
for (const path of files)
  if (!/^[\w-][\w./-]*$/.test(path) || path.includes("..") || path.includes("//"))
    fail(`${path} can't be one of a mod's files: letters, numbers, - _ . and /, and nothing hidden`);

const sha512 = (bytes) => createHash("sha512").update(bytes).digest("hex");
const lines = ["kobo-mod-manifest\t1", ["mod", sha512(data), data.length, targets, version, release ? "release" : "dev"].join("\t")];
mkdirSync(out, { recursive: true });
for (const path of files) {
  const bytes = readFileSync(join(resolve(values.files), path));
  lines.push(["file", path, sha512(bytes), bytes.length].join("\t"));
  mkdirSync(dirname(join(out, path)), { recursive: true });
  writeFileSync(join(out, path), bytes);
}
const manifest = Buffer.from(lines.join("\n") + "\n");
copyFileSync(so, join(out, `lib${name}.so`));
writeFileSync(join(out, "manifest.txt"), manifest);
writeFileSync(join(out, "manifest.sig"), sign(null, manifest, key).toString("hex") + "\n");
console.log(`signed ${name} ${version}, a ${release ? "release" : "dev build"}, for ${targets}, ${files.length ? `with ${files.length} files, ` : ""}in ${out}`);
