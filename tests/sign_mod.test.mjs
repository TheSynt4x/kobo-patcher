// Run with: make test
// tools/sign_mod.mjs, on a made-up mod: what it signs, and what it refuses.
import assert from "node:assert/strict";
import { execFileSync, spawnSync } from "node:child_process";
import { createHash, createPublicKey, verify } from "node:crypto";
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { after, test } from "node:test";
import { fileURLToPath } from "node:url";

const tool = join(dirname(dirname(fileURLToPath(import.meta.url))), "tools/sign_mod.mjs");
const work = mkdtempSync(join(tmpdir(), "sign-mod-test-"));
after(() => rmSync(work, { recursive: true, force: true }));

const run = (...args) => spawnSync("node", [tool, ...args], { encoding: "utf8" });
const key = join(work, "key.pem");
const publicKey = execFileSync("node", [tool, "--new-key", key], { encoding: "utf8" }).match(/key = "([0-9a-f]{64})"/)[1];

// A mod folder with build/lib<mod>.so beside it, stamped as build.sh stamps it.
function mod(name, { version = "3", stamp = "release", script = `nk.updates{url = "https://example.com/", key = "${publicKey}"}` } = {}) {
  const folder = join(work, name);
  mkdirSync(folder, { recursive: true });
  mkdirSync(join(work, "build"), { recursive: true });
  writeFileSync(join(folder, "main.lua"), script);
  if (version !== null) writeFileSync(join(folder, "version"), version + "\n");
  writeFileSync(join(work, "build", `lib${name}.so`), Buffer.concat([Buffer.from("\x7fELF stand-in\0nk-build: "), Buffer.from(stamp), Buffer.from("\0rest")]));
  return folder;
}

test("signs a release, for the targets given", () => {
  const out = join(work, "out-release");
  const result = run(mod("good"), out, "--key", key, "--targets", "N306-4.38.23852");
  assert.equal(result.status, 0, result.stderr);
  const so = readFileSync(join(out, "libgood.so"));
  const manifest = readFileSync(join(out, "manifest.txt"));
  const hash = createHash("sha512").update(so).digest("hex");
  assert.equal(manifest.toString(), `kobo-mod-manifest\t1\nmod\t${hash}\t${so.length}\tN306-4.38.23852\t3\trelease\n`);
  const signature = Buffer.from(readFileSync(join(out, "manifest.sig"), "utf8").trim(), "hex");
  assert.ok(verify(null, manifest, createPublicKey(readFileSync(key)), signature));
});

test("says a dev build is one", () => {
  const out = join(work, "out-dev");
  assert.equal(run(mod("devmod", { stamp: "dev 1234567 2026-11-08 17:00" }), out, "--key", key).status, 0);
  assert.match(readFileSync(join(out, "manifest.txt"), "utf8"), /\t3\tdev\n$/);
});

test("refuses a key the mod doesn't have", () => {
  const other = join(work, "other.pem");
  execFileSync("node", [tool, "--new-key", other]);
  const result = run(mod("wrongkey"), join(work, "x"), "--key", other);
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /isn't in any of wrongkey's scripts/);
});

test("refuses a mod without a version, or with one that isn't a whole number", () => {
  assert.match(run(mod("noversion", { version: null }), join(work, "x"), "--key", key).stderr, /needs a version/);
  assert.match(run(mod("badversion", { version: "1.2" }), join(work, "x"), "--key", key).stderr, /whole number/);
});

test("refuses a build without a stamp, and bad targets", () => {
  const folder = mod("nostamp");
  writeFileSync(join(work, "build", "libnostamp.so"), "\x7fELF without one");
  assert.match(run(folder, join(work, "x"), "--key", key).stderr, /no build stamp/);
  assert.match(run(mod("targets"), join(work, "x"), "--key", key, "--targets", "N306 4.38").stderr, /model-firmware/);
});

test("needs a key", () => {
  const result = spawnSync("node", [tool, mod("nokey"), join(work, "x")], { encoding: "utf8", env: { ...process.env, KOBO_MOD_SIGNING_KEY: "" } });
  assert.match(result.stderr, /no signing key/);
});
