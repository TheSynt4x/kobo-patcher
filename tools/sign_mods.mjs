#!/usr/bin/env node
// Write web/mods/manifest.txt and its signature, which Kobo Patcher downloads to check for updates.
//
//   node tools/sign_mods.mjs            sign what make mods built
//   node tools/sign_mods.mjs --new-key  make a signing key, once
//
// The manifest names each file with its SHA-512 and size, and the mod only uses a file that matches:
//
//   kobo-patcher-manifest	1
//   mod	<sha512>	<size>	<model-firmware,...>	<version>     the Kobos the mod updates itself on
//   list	<model-firmware>	<sha512>	<size>
//   list2	<model-firmware>	<sha512>	<size>                    the same list in format 2, in lists/2
//
// The version is update.VERSION in mods/patches/update.lua. A Kobo is only offered the mod when it's
// higher than its own. The C mod before the Lua one reads only the first four fields.
//
// manifest.sig is the manifest's Ed25519 signature, in hex. The key comes from the environment's
// KOBO_PATCHER_SIGNING_KEY, or from ~/.config/kobo-patcher/signing-key.pem. Its public half has to
// be the UPDATE_KEY built into the mod, or nothing is signed: Kobos would refuse it.
import { createHash, createPrivateKey, createPublicKey, generateKeyPairSync, sign } from "node:crypto";
import { existsSync, mkdirSync, readdirSync, readFileSync, writeFileSync } from "node:fs";
import { homedir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { FIRMWARE } from "../web/patches/compiled.js";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..");
const MODS = join(ROOT, "web/mods");
const KEY_FILE = join(homedir(), ".config/kobo-patcher/signing-key.pem");
const SOURCE = join(ROOT, "mods/patches/update.lua");

const rawPublicKey = (key) => createPublicKey(key).export({ format: "der", type: "spki" }).subarray(-32).toString("hex");

if (process.argv[2] === "--new-key") {
  if (existsSync(KEY_FILE)) throw new Error(`${KEY_FILE} already exists`);
  const { privateKey } = generateKeyPairSync("ed25519");
  mkdirSync(dirname(KEY_FILE), { recursive: true });
  writeFileSync(KEY_FILE, privateKey.export({ format: "pem", type: "pkcs8" }), { mode: 0o600 });
  console.log(`wrote ${KEY_FILE}. Keep it secret, and put its public half in ${SOURCE}:`);
  console.log(`local UPDATE_KEY = "${rawPublicKey(privateKey)}"`);
  process.exit(0);
}

const pem = process.env.KOBO_PATCHER_SIGNING_KEY || (existsSync(KEY_FILE) && readFileSync(KEY_FILE, "utf8"));
if (!pem) {
  // Deploying without it would leave Kobos with nothing to update from.
  if (process.argv.includes("--require")) throw new Error("no signing key: set KOBO_PATCHER_SIGNING_KEY");
  console.log("no signing key, so web/mods/manifest.txt isn't written");
  process.exit(0);
}
const key = createPrivateKey(pem);
const expected = readFileSync(SOURCE, "utf8").match(/local UPDATE_KEY = "([0-9a-f]{64})"/)?.[1];
if (rawPublicKey(key) !== expected) throw new Error(`the signing key doesn't match UPDATE_KEY in ${SOURCE}`);
const version = readFileSync(SOURCE, "utf8").match(/^update\.VERSION = (\d+)$/m)?.[1];
if (!version) throw new Error(`no update.VERSION in ${SOURCE}`);

const describe = (path) => {
  const data = readFileSync(path);
  return [createHash("sha512").update(data).digest("hex"), data.length];
};
const targets = FIRMWARE.filter((build) => build.mod).map((build) => `${build.model}-${build.firmware}`);
const lines = ["kobo-patcher-manifest\t1", ["mod", ...describe(join(MODS, "libpatches.so")), targets.join(","), version].join("\t")];
for (const file of readdirSync(join(MODS, "lists")).filter((name) => name.endsWith(".txt")).sort())
  lines.push(["list", file.slice(0, -4), ...describe(join(MODS, "lists", file))].join("\t"));
// Older Kobo Patchers skip list2 lines, and keep reading format 1.
for (const file of readdirSync(join(MODS, "lists", "2")).filter((name) => name.endsWith(".txt")).sort())
  lines.push(["list2", file.slice(0, -4), ...describe(join(MODS, "lists", "2", file))].join("\t"));
const manifest = Buffer.from(lines.join("\n") + "\n");
writeFileSync(join(MODS, "manifest.txt"), manifest);
writeFileSync(join(MODS, "manifest.sig"), sign(null, manifest, key).toString("hex") + "\n");
console.log(`signed web/mods/manifest.txt: the mod for ${targets.join(", ") || "no Kobo"}, and ${lines.length - 2} patch lists, in both formats`);
