// The patching logic behind index.html, with no page code so Node can test it.

import { FIRMWARE, KOREADER, MODEL_NAMES, NICKELMENU, REGISTRY } from "./supported.js";

const NICKEL_PATH = "./usr/local/Kobo/libnickel.so.1.0.0";
const NICKEL_BIN_PATH = "./usr/local/Kobo/nickel";
const RECORD = "kobo-patcher.json";
const DRIVE_CHECK = "_ZNK6Device19supportsGoogleDriveEv";
const DRIVE_CHECK_START = [0x98, 0xb5, 0x01, 0x21, 0x00, 0xaf];
const DRIVE_CHECK_TRUE = [0x00, 0xb5, 0x01, 0x20, 0x00, 0xbd]; // push {lr}; movs r0,#1; pop {pc}
const DRIVE_SETTINGS = [
  ["FeatureSettings", "GoogleDrive", "1"],
  ["OneStoreServices", "kobo_googledrive_link_account_enabled", "True"],
  ["OneStoreServices", "googledrive_link_account_start",
    "https://authorize.kobo.com/{region}/{language}/linkcloudstorage/provider/google_drive"],
];

// A firmware is known by the hash of its libnickel, so a renamed zip still works
// and a different build with the same version number doesn't.
export const SUPPORTED = Object.fromEntries(
  FIRMWARE.filter((build) => build.nickelSha256).map((build) => [build.nickelSha256, build]),
);

const KOREADER_MENU = "menu_item:main:KOReader:cmd_spawn:quiet:exec /mnt/onboard/.adds/koreader/koreader.sh\n";

const PATCH_CODE = { "google-drive": { apply: patchDriveCheck, settings: DRIVE_SETTINGS } };

export const PATCHES = REGISTRY.filter((item) => item.kind === "patch")
  .map((item) => ({ settings: [], ...item, ...PATCH_CODE[item.id] }));

export class Refused extends Error {}

// Says what to ask for, so the page can link to a request for this firmware.
export class UnsupportedFirmware extends Refused {
  constructor(message, model, firmware) {
    super(message);
    Object.assign(this, { model, firmware });
  }
}

const encoder = new TextEncoder();

function concat(chunks, size) {
  const out = new Uint8Array(size ?? chunks.reduce((sum, c) => sum + c.length, 0));
  let at = 0;
  for (const chunk of chunks) {
    out.set(chunk, at);
    at += chunk.length;
  }
  return out;
}

function indexOfBytes(haystack, needle) {
  outer: for (let i = 0; i <= haystack.length - needle.length; i++) {
    for (let j = 0; j < needle.length; j++) {
      if (haystack[i + j] !== needle[j]) continue outer;
    }
    return i;
  }
  return -1;
}

export async function sha256(bytes) {
  const digest = await crypto.subtle.digest("SHA-256", bytes);
  return Array.from(new Uint8Array(digest), (b) => b.toString(16).padStart(2, "0")).join("");
}

async function readBlob(blob) {
  return new Uint8Array(await blob.arrayBuffer());
}

async function zipEntries(file) {
  const tail = await readBlob(file.slice(Math.max(0, file.size - 66000)));
  const view = new DataView(tail.buffer);
  let end = tail.length - 22;
  while (end >= 0 && view.getUint32(end, true) !== 0x06054b50) end--;
  if (end < 0) throw new Refused("this file isn't a zip");

  const count = view.getUint16(end + 10, true);
  const dirSize = view.getUint32(end + 12, true);
  const dirOffset = view.getUint32(end + 16, true);
  const dir = await readBlob(file.slice(dirOffset, dirOffset + dirSize));
  const entries = new DataView(dir.buffer);
  const out = [];
  for (let at = 0, i = 0; i < count; i++) {
    const nameLength = entries.getUint16(at + 28, true);
    out.push({
      name: new TextDecoder().decode(dir.subarray(at + 46, at + 46 + nameLength)),
      method: entries.getUint16(at + 10, true),
      size: entries.getUint32(at + 20, true),
      offset: entries.getUint32(at + 42, true),
    });
    at += 46 + nameLength + entries.getUint16(at + 30, true) + entries.getUint16(at + 32, true);
  }
  return out;
}

async function zipEntryStream(file, entry) {
  const local = new DataView((await readBlob(file.slice(entry.offset, entry.offset + 30))).buffer);
  const start = entry.offset + 30 + local.getUint16(26, true) + local.getUint16(28, true);
  const data = file.slice(start, start + entry.size).stream();
  if (entry.method === 0) return data;
  if (entry.method === 8) return data.pipeThrough(new DecompressionStream("deflate-raw"));
  throw new Refused(`${entry.name} uses a kind of compression this page can't read`);
}

function tarText(block, start, length) {
  const field = block.subarray(start, start + length);
  const stop = field.indexOf(0);
  return new TextDecoder().decode(stop < 0 ? field : field.subarray(0, stop));
}

// Reads the stream only as far as the files, because KoboRoot.tgz is far larger than they are.
async function filesFromTar(stream, paths) {
  const found = {};
  const reader = stream.getReader();
  let chunk = new Uint8Array(0);
  let at = 0;
  async function take(size, keep) {
    const out = keep ? new Uint8Array(size) : null;
    let have = 0;
    while (have < size) {
      if (at === chunk.length) {
        const { done, value } = await reader.read();
        if (done) return null;
        chunk = value;
        at = 0;
      }
      const part = Math.min(size - have, chunk.length - at);
      if (keep) out.set(chunk.subarray(at, at + part), have);
      at += part;
      have += part;
    }
    return keep ? out : true;
  }
  try {
    for (;;) {
      const header = await take(512, true);
      if (!header || header.every((b) => b === 0)) return found;
      const name = tarText(header, 0, 100);
      const prefix = tarText(header, 345, 155);
      const size = parseInt(tarText(header, 124, 12).trim() || "0", 8);
      const type = String.fromCharCode(header[156]);
      const path = prefix ? `${prefix}/${name}` : name;
      if (paths.includes(path) && (type === "0" || type === "\0")) {
        found[path] = await take(size, true);
        if (paths.every((p) => found[p])) return found;
        if (await take(Math.ceil(size / 512) * 512 - size, false) === null) return found;
        continue;
      }
      if (await take(Math.ceil(size / 512) * 512, false) === null) return found;
    }
  } finally {
    reader.cancel().catch(() => {});
  }
}

async function readFirmwareFiles(file, paths) {
  try {
    const entry = (await zipEntries(file)).find((e) => e.name === "KoboRoot.tgz");
    if (!entry) throw new Refused("no KoboRoot.tgz in this zip, so it isn't a Kobo firmware update");
    const root = await zipEntryStream(file, entry);
    return await filesFromTar(root.pipeThrough(new DecompressionStream("gzip")), paths);
  } catch (error) {
    if (error instanceof Refused) throw error;
    throw new Refused(`couldn't read this firmware file: ${error.message}`);
  }
}

export async function readNickel(file) {
  const nickel = (await readFirmwareFiles(file, [NICKEL_PATH]))[NICKEL_PATH];
  if (!nickel) throw new Refused("no libnickel in this file, so it isn't a Kobo firmware update");
  return nickel;
}

export async function readFirmware(file) {
  const found = await readFirmwareFiles(file, [NICKEL_PATH, NICKEL_BIN_PATH]);
  if (!found[NICKEL_PATH] || !found[NICKEL_BIN_PATH]) {
    throw new Refused("this file is missing Kobo's system files, so it isn't a firmware update");
  }
  return { libnickel: found[NICKEL_PATH], nickel: found[NICKEL_BIN_PATH] };
}

async function knownRelease(file, releases, name) {
  const bytes = await readBlob(file);
  const hash = await sha256(bytes);
  const release = releases.find((r) => r.sha256 === hash);
  if (!release) {
    throw new Refused(`this isn't a supported ${name} file. Supported: ${releases.map((r) => r.version).join(", ")}`);
  }
  return { release, bytes };
}

// The official KoboRoot.tgz, as a list of files to merge into the one package a Kobo installs.
export async function readNickelMenu(file) {
  const { release, bytes } = await knownRelease(file, NICKELMENU, "NickelMenu");
  const gunzip = new Blob([bytes]).stream().pipeThrough(new DecompressionStream("gzip"));
  const tar = new Uint8Array(await new Response(gunzip).arrayBuffer());
  const files = [];
  for (let at = 0; at + 512 <= tar.length && tar[at] !== 0;) {
    const header = tar.subarray(at, at + 512);
    const size = parseInt(tarText(header, 124, 12).trim() || "0", 8);
    files.push({
      path: tarText(header, 0, 100),
      mode: parseInt(tarText(header, 100, 8).trim(), 8),
      data: tar.slice(at + 512, at + 512 + size),
    });
    at += 512 + Math.ceil(size / 512) * 512;
  }
  return { version: release.version, files };
}

export async function readKOReader(file) {
  const { release } = await knownRelease(file, KOREADER, "KOReader");
  const entries = (await zipEntries(file)).filter((e) => e.name.startsWith("koreader/") && !e.name.endsWith("/"));
  return { version: release.version, file, entries };
}

// Where an exported function starts in a 32-bit little-endian ELF file, or null.
export function functionOffset(elf, name) {
  const magic = [0x7f, 0x45, 0x4c, 0x46, 0x01, 0x01];
  if (!magic.every((b, i) => elf[i] === b)) {
    throw new Refused("libnickel isn't the 32-bit ARM file this patch expects");
  }
  const view = new DataView(elf.buffer, elf.byteOffset, elf.byteLength);
  const u16 = (at) => view.getUint16(at, true);
  const u32 = (at) => view.getUint32(at, true);
  const [phoff, shoff] = [u32(28), u32(32)];
  const [phsize, phnum, shsize, shnum] = [u16(42), u16(44), u16(46), u16(48)];
  const section = (i) => {
    const at = shoff + i * shsize;
    return { kind: u32(at + 4), offset: u32(at + 16), size: u32(at + 20), link: u32(at + 24) };
  };
  let dynsym = null;
  for (let i = 0; i < shnum && !dynsym; i++) {
    if (section(i).kind === 11) dynsym = section(i);
  }
  if (!dynsym) return null;
  const strings = section(dynsym.link);
  const table = elf.subarray(strings.offset, strings.offset + strings.size);
  const found = indexOfBytes(table, encoder.encode(`\0${name}\0`));
  if (found < 0) return null;
  for (let symbol = dynsym.offset; symbol < dynsym.offset + dynsym.size; symbol += 16) {
    if (u32(symbol) !== found + 1) continue;
    const address = u32(symbol + 4) & ~1; // the low bit only marks Thumb code
    for (let i = 0; i < phnum; i++) {
      const at = phoff + i * phsize;
      const [kind, offset, vaddr, filesz] = [u32(at), u32(at + 4), u32(at + 8), u32(at + 16)];
      if (kind === 1 && vaddr <= address && address < vaddr + filesz) return address - vaddr + offset;
    }
  }
  return null;
}

// Make the device-model check for Google Drive return true. Refuses code it doesn't know.
export function patchDriveCheck(nickel) {
  const offset = functionOffset(nickel, DRIVE_CHECK);
  if (offset === null) throw new Refused("this firmware has no Google Drive device check to patch");
  if (!DRIVE_CHECK_START.every((b, i) => nickel[offset + i] === b)) {
    throw new Refused("the Google Drive check has changed in this firmware, so it needs a new patch");
  }
  const out = nickel.slice();
  out.set(DRIVE_CHECK_TRUE, offset);
  return out;
}

// Set key=value in an INI-style section, keeping the file's other lines.
export function setConf(text, section, key, value) {
  const newline = text.includes("\r\n") ? "\r\n" : "\n";
  const line = `${key}=${value}`;
  const start = text.indexOf(`[${section}]`);
  if (start < 0) return `${text.trimEnd()}${newline}${newline}[${section}]${newline}${line}${newline}`;
  const next = text.indexOf(`${newline}[`, start + 1);
  const end = next >= 0 ? next : text.length;
  const body = text.slice(start, end);
  const pattern = new RegExp(`^${key.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")}=.*$`, "m");
  if (pattern.test(body)) return text.slice(0, start) + body.replace(pattern, () => line) + text.slice(end);
  const header = `[${section}]${newline}`;
  return text.slice(0, start) + header + line + newline + text.slice(start + header.length);
}

function octal(value, width) {
  return value.toString(8).padStart(width - 1, "0") + "\0";
}

function tarHeader(path, size, mode, mtime) {
  const header = new Uint8Array(512);
  const put = (at, text) => header.set(encoder.encode(text), at);
  put(0, path);
  put(100, octal(mode, 8));
  put(108, octal(0, 8));
  put(116, octal(0, 8));
  put(124, octal(size, 12));
  put(136, octal(mtime, 12));
  put(148, "        ");
  put(156, "0");
  put(257, "ustar\x0000");
  put(265, "root");
  put(297, "root");
  const sum = header.reduce((total, b) => total + b, 0);
  put(148, `${sum.toString(8).padStart(6, "0")}\0 `);
  return header;
}

// A package holding these files. No directory entries, so their folders keep their modes.
export async function buildPackage(files, mtime = Math.floor(Date.now() / 1000)) {
  const used = files.reduce((sum, f) => sum + 512 + Math.ceil(f.data.length / 512) * 512, 1024);
  const tar = new Uint8Array(Math.ceil(used / 10240) * 10240);
  let at = 0;
  for (const { path, data, mode } of files) {
    tar.set(tarHeader(path, data.length, mode, mtime), at);
    tar.set(data, at + 512);
    at += 512 + Math.ceil(data.length / 512) * 512;
  }
  const gzip = new Blob([tar]).stream().pipeThrough(new CompressionStream("gzip"));
  return new Uint8Array(await new Response(gzip).arrayBuffer());
}

const crcTable = Array.from({ length: 256 }, (_, n) => {
  for (let k = 0; k < 8; k++) n = n & 1 ? 0xedb88320 ^ (n >>> 1) : n >>> 1;
  return n >>> 0;
});

function crcUpdate(crc, bytes) {
  for (const byte of bytes) crc = crcTable[(crc ^ byte) & 0xff] ^ (crc >>> 8);
  return crc;
}

function crc32(bytes) {
  return (crcUpdate(0xffffffff, bytes) ^ 0xffffffff) >>> 0;
}

// An uncompressed zip, which is enough for a backup of two files.
export function buildZip(files) {
  const parts = [];
  const directory = [];
  let offset = 0;
  for (const { name, data } of files) {
    const nameBytes = encoder.encode(name);
    const common = new DataView(new ArrayBuffer(26));
    common.setUint16(0, 20, true);
    common.setUint16(2, 0x0800, true); // the name is UTF-8
    common.setUint32(10, crc32(data), true);
    common.setUint32(14, data.length, true);
    common.setUint32(18, data.length, true);
    common.setUint16(22, nameBytes.length, true);
    const shared = new Uint8Array(common.buffer);

    const local = new Uint8Array(30 + nameBytes.length);
    new DataView(local.buffer).setUint32(0, 0x04034b50, true);
    local.set(shared, 4);
    local.set(nameBytes, 30);
    parts.push(local, data);

    const entry = new Uint8Array(46 + nameBytes.length);
    const view = new DataView(entry.buffer);
    view.setUint32(0, 0x02014b50, true);
    view.setUint16(4, 20, true);
    entry.set(shared, 6);
    view.setUint32(42, offset, true);
    entry.set(nameBytes, 46);
    directory.push(entry);
    offset += local.length + data.length;
  }
  const dirSize = directory.reduce((sum, e) => sum + e.length, 0);
  const end = new Uint8Array(22);
  const view = new DataView(end.buffer);
  view.setUint32(0, 0x06054b50, true);
  view.setUint16(8, files.length, true);
  view.setUint16(10, files.length, true);
  view.setUint32(12, dirSize, true);
  view.setUint32(16, offset, true);
  return concat([...parts, ...directory, end]);
}

const ZIP64_AT = 0xffffffff;

function dosDateTime(ms) {
  const d = new Date(ms);
  const year = Math.max(d.getFullYear(), 1980);
  return {
    time: (d.getHours() << 11) | (d.getMinutes() << 5) | (d.getSeconds() >> 1),
    date: ((year - 1980) << 9) | ((d.getMonth() + 1) << 5) | d.getDate(),
  };
}

// Streams files into an uncompressed zip, without holding them in memory, so a whole Kobo fits.
// Each CRC is only known after its file is read, so it goes back and fills that in.
// entries: [{ name, open }], where open() gives a File. sink: { write(bytes), patch(position, bytes) }.
export async function writeZip(entries, sink, progress = () => {}, limit = ZIP64_AT) {
  const central = [];
  let position = 0;
  const emit = async (bytes) => {
    await sink.write(bytes);
    position += bytes.length;
  };
  let done = 0;
  for (const entry of entries) {
    const file = await entry.open();
    const nameBytes = encoder.encode(entry.name);
    const big = file.size >= limit;
    const offset = position;
    const { time, date } = dosDateTime(file.lastModified);

    const local = new Uint8Array(30 + nameBytes.length + (big ? 20 : 0));
    const view = new DataView(local.buffer);
    view.setUint32(0, 0x04034b50, true);
    view.setUint16(4, big ? 45 : 20, true);
    view.setUint16(6, 0x0800, true); // the name is UTF-8
    view.setUint16(10, time, true);
    view.setUint16(12, date, true);
    view.setUint32(18, big ? ZIP64_AT : file.size, true);
    view.setUint32(22, big ? ZIP64_AT : file.size, true);
    view.setUint16(26, nameBytes.length, true);
    view.setUint16(28, big ? 20 : 0, true);
    local.set(nameBytes, 30);
    if (big) {
      const at = 30 + nameBytes.length;
      view.setUint16(at, 1, true);
      view.setUint16(at + 2, 16, true);
      view.setBigUint64(at + 4, BigInt(file.size), true);
      view.setBigUint64(at + 12, BigInt(file.size), true);
    }
    await emit(local);

    let crc = 0xffffffff;
    let read = 0;
    const reader = file.stream().getReader();
    for (;;) {
      const { done: finished, value } = await reader.read();
      if (finished) break;
      crc = crcUpdate(crc, value);
      read += value.length;
      await emit(value);
    }
    if (read !== file.size) throw new Refused(`${entry.name} changed while it was being read`);
    crc = (crc ^ 0xffffffff) >>> 0;
    const crcBytes = new Uint8Array(4);
    new DataView(crcBytes.buffer).setUint32(0, crc, true);
    await sink.patch(offset + 14, crcBytes);

    central.push({ nameBytes, size: file.size, offset, crc, time, date });
    done += 1;
    progress({ done, total: entries.length, bytes: position });
  }

  const dirOffset = position;
  for (const e of central) {
    const fields = [e.size >= limit && e.size, e.size >= limit && e.size, e.offset >= limit && e.offset]
      .filter((v) => v !== false);
    const extra = fields.length ? 4 + fields.length * 8 : 0;
    const record = new Uint8Array(46 + e.nameBytes.length + extra);
    const view = new DataView(record.buffer);
    view.setUint32(0, 0x02014b50, true);
    view.setUint16(4, extra ? 45 : 20, true);
    view.setUint16(6, extra ? 45 : 20, true);
    view.setUint16(8, 0x0800, true);
    view.setUint16(12, e.time, true);
    view.setUint16(14, e.date, true);
    view.setUint32(16, e.crc, true);
    view.setUint32(20, e.size >= limit ? ZIP64_AT : e.size, true);
    view.setUint32(24, e.size >= limit ? ZIP64_AT : e.size, true);
    view.setUint16(28, e.nameBytes.length, true);
    view.setUint16(30, extra, true);
    view.setUint32(42, e.offset >= limit ? ZIP64_AT : e.offset, true);
    record.set(e.nameBytes, 46);
    if (extra) {
      const at = 46 + e.nameBytes.length;
      view.setUint16(at, 1, true);
      view.setUint16(at + 2, fields.length * 8, true);
      fields.forEach((v, i) => view.setBigUint64(at + 4 + i * 8, BigInt(v), true));
    }
    await emit(record);
  }

  const dirSize = position - dirOffset;
  const big = central.length >= 0xffff || dirOffset >= limit || dirSize >= limit;
  if (big) {
    const end64 = new Uint8Array(76);
    const view = new DataView(end64.buffer);
    view.setUint32(0, 0x06064b50, true);
    view.setBigUint64(4, 44n, true);
    view.setUint16(12, 45, true);
    view.setUint16(14, 45, true);
    view.setBigUint64(24, BigInt(central.length), true);
    view.setBigUint64(32, BigInt(central.length), true);
    view.setBigUint64(40, BigInt(dirSize), true);
    view.setBigUint64(48, BigInt(dirOffset), true);
    view.setUint32(56, 0x07064b50, true);
    view.setBigUint64(64, BigInt(position), true);
    view.setUint32(72, 1, true);
    await emit(end64);
  }
  const end = new Uint8Array(22);
  const view = new DataView(end.buffer);
  view.setUint32(0, 0x06054b50, true);
  view.setUint16(8, big ? 0xffff : central.length, true);
  view.setUint16(10, big ? 0xffff : central.length, true);
  view.setUint32(12, big ? ZIP64_AT : dirSize, true);
  view.setUint32(16, big ? ZIP64_AT : dirOffset, true);
  await emit(end);
}

export function parseVersion(text) {
  const fields = text.trim().split(",");
  if (fields.length < 3) throw new Refused("this Kobo's version file isn't in the expected format");
  return { serial: fields[0], firmware: fields[2], model: fields[0].slice(0, 4) };
}

async function exists(dir, name) {
  try {
    await dir.getFileHandle(name);
    return true;
  } catch (error) {
    if (error.name === "NotFoundError") return false;
    if (error.name === "TypeMismatchError") return true;
    throw error;
  }
}

async function dirExists(dir, name) {
  try {
    await dir.getDirectoryHandle(name);
    return true;
  } catch (error) {
    if (error.name === "NotFoundError") return false;
    if (error.name === "TypeMismatchError") return true;
    throw error;
  }
}

async function readFile(dir, name) {
  return readBlob(await (await dir.getFileHandle(name)).getFile());
}

async function writeFile(dir, name, data) {
  const writable = await (await dir.getFileHandle(name, { create: true })).createWritable();
  await writable.write(data);
  await writable.close();
}

// macOS leaves ._ files on the Kobo's drive, and the Kobo tries to read them as real ones.
async function cleanMacFiles(dir, deep = false) {
  for await (const [name, handle] of dir.entries()) {
    if (name.startsWith("._")) await dir.removeEntry(name);
    else if (deep && handle.kind === "directory") await cleanMacFiles(handle, true);
  }
}

// Reads a chosen folder and says what Kobo it is. Refuses anything but an idle, supported one.
export async function inspectKobo(root) {
  let dot;
  let version;
  try {
    dot = await root.getDirectoryHandle(".kobo");
    version = parseVersion(new TextDecoder().decode(await readFile(dot, "version")));
  } catch (error) {
    if (error instanceof Refused) throw error;
    if (error.name !== "NotFoundError") throw error;
    throw new Refused("no Kobo in that folder: choose the KOBOeReader drive itself");
  }
  const name = MODEL_NAMES[version.model] ?? `Kobo model ${version.model}`;
  const build = FIRMWARE.find((b) => b.firmware === version.firmware && b.model === version.model);
  if (!build) {
    throw new UnsupportedFirmware(`found a ${name} on ${version.firmware}, which isn't supported yet. `
      + "Press the ? to see what is", version.model, version.firmware);
  }
  if (await exists(dot, "KoboRoot.tgz") || await dirExists(dot, "upgrade") || await exists(dot, "upgrade")) {
    throw new Refused("an update package is already waiting on this Kobo. Eject it, let it "
      + "install, then try again");
  }
  let adds = null;
  if (await dirExists(root, ".adds")) adds = await root.getDirectoryHandle(".adds");
  let patches = [];
  if (adds && await exists(adds, RECORD)) {
    try {
      const record = JSON.parse(new TextDecoder().decode(await readFile(adds, RECORD)));
      // A firmware update puts the original files back, so an older record no longer applies.
      if (record.firmware === version.firmware) patches = record.patches;
    } catch {
      patches = [];
    }
  }
  const installed = {
    // An empty nm folder is what's left after NickelMenu uninstalls itself.
    nickelmenu: !!adds && await dirExists(adds, "nm")
      && await exists(await adds.getDirectoryHandle("nm"), "doc")
      && !await exists(await adds.getDirectoryHandle("nm"), "uninstall"),
    patches,
    koreader: !!adds && await dirExists(adds, "koreader"),
  };
  return { ...version, name, build, installed };
}

function fromHex(text) {
  return Uint8Array.from(text.match(/../g) ?? [], (pair) => parseInt(pair, 16));
}

// Applies byte edits, checking each against what it expects. Two patches that change the same
// bytes fail here too, because the second no longer finds what it expects.
function applyEdits(data, edits, title) {
  const out = data.slice();
  for (const { at, find, replace } of edits) {
    const expected = fromHex(find);
    if (!expected.every((byte, i) => out[at + i] === byte)) {
      throw new Refused(`"${title}" doesn't fit this firmware, or clashes with another patch you chose`);
    }
    out.set(fromHex(replace), at);
  }
  return out;
}

function systemFiles(firmware) {
  const files = [{ path: NICKEL_PATH, data: firmware.libnickel, mode: 0o775 }];
  if (firmware.nickel) files.push({ path: NICKEL_BIN_PATH, data: firmware.nickel, mode: 0o775 });
  return files;
}

// Works everything out before writing anything, so a patch that doesn't fit leaves the Kobo alone.
// Pass the firmware only to change patches: both system files are rebuilt from its originals, so
// a patch that's no longer chosen goes away. Without it the system files aren't touched.
export function planPatch(firmware, patchIds, { nickelmenu = null, koreader = null, edits = {}, remove = null } = {}) {
  const chosen = PATCHES.filter((patch) => patchIds.includes(patch.id));
  if (chosen.length && !firmware) throw new Refused("patches need the firmware file");
  if (koreader && !nickelmenu) throw new Refused("KOReader's menu entry needs NickelMenu");
  const files = [];
  if (firmware) {
    const current = { [NICKEL_PATH]: firmware.libnickel, [NICKEL_BIN_PATH]: firmware.nickel };
    for (const patch of chosen) {
      if (patch.apply) {
        current[NICKEL_PATH] = patch.apply(current[NICKEL_PATH]);
        continue;
      }
      if (!edits[patch.id]) throw new Refused(`"${patch.title}" isn't available for this firmware`);
      for (const path of new Set(edits[patch.id].map((edit) => edit.file))) {
        const part = edits[patch.id].filter((edit) => edit.file === path);
        current[path] = applyEdits(current[path], part, patch.title);
      }
    }
    files.push(...systemFiles({ libnickel: current[NICKEL_PATH], nickel: current[NICKEL_BIN_PATH] }));
  }
  if (nickelmenu) files.push(...nickelmenu.files);
  return {
    settings: chosen.flatMap((patch) => patch.settings),
    files,
    koreader,
    remove,
    installsNickelMenu: !!nickelmenu,
    record: firmware ? patchIds : null,
  };
}

// Removes what the page can add, and puts Kobo's original system files back if given the firmware.
export function planUndo(firmware, installed) {
  return {
    settings: [],
    files: firmware ? systemFiles(firmware) : [],
    koreader: null,
    remove: installed,
    record: firmware ? [] : null,
  };
}

async function copyKOReader(root, koreader, progress) {
  const adds = await root.getDirectoryHandle(".adds", { create: true });
  const folders = new Map();
  let done = 0;
  for (const entry of koreader.entries) {
    const parts = entry.name.split("/");
    const name = parts.pop();
    const key = parts.join("/");
    if (!folders.has(key)) {
      let dir = adds;
      for (const part of parts) dir = await dir.getDirectoryHandle(part, { create: true });
      folders.set(key, dir);
    }
    const stream = await zipEntryStream(koreader.file, entry);
    await writeFile(folders.get(key), name, await new Response(stream).blob());
    done += 1;
    if (done % 25 === 0 || done === koreader.entries.length) progress(`Copying KOReader: ${done} of ${koreader.entries.length} files…`);
  }
  await cleanMacFiles(await adds.getDirectoryHandle("koreader"), true);
  const nm = await adds.getDirectoryHandle("nm", { create: true });
  await writeFile(nm, "koreader", encoder.encode(KOREADER_MENU));
  await cleanMacFiles(nm);
}

async function removeAddons(root, remove) {
  const adds = await root.getDirectoryHandle(".adds");
  await removeFromAdds(adds, remove);
  if (remove.koreader && await dirExists(adds, "koreader")) {
    throw new Refused("KOReader's folder is still on the Kobo after removing it");
  }
  if (remove.nickelmenu && !await exists(await adds.getDirectoryHandle("nm"), "uninstall")) {
    throw new Refused("couldn't leave NickelMenu's uninstall file on the Kobo");
  }
}

async function removeFromAdds(adds, remove) {
  if (remove.koreader) await adds.removeEntry("koreader", { recursive: true });
  if (!await dirExists(adds, "nm")) return;
  const nm = await adds.getDirectoryHandle("nm");
  if (remove.koreader && await exists(nm, "koreader")) await nm.removeEntry("koreader");
  // NickelMenu removes itself when it finds this file on the next restart.
  if (remove.nickelmenu) await writeFile(nm, "uninstall", new Uint8Array(0));
  await cleanMacFiles(nm);
}

// Folders to leave out of a backup: the computer's, not the Kobo's.
const SKIP_IN_FULL_BACKUP = new Set([".Spotlight-V100", ".fseventsd", ".Trashes", "System Volume Information"]);

async function listFiles(dir, prefix = "") {
  const out = [];
  for await (const [name, handle] of dir.entries()) {
    if (!prefix && SKIP_IN_FULL_BACKUP.has(name)) continue;
    if (handle.kind === "directory") out.push(...await listFiles(handle, `${prefix}${name}/`));
    else out.push({ name: `${prefix}${name}`, open: () => handle.getFile() });
  }
  return out;
}

// Reads a chosen folder just enough to name a backup. Works for any Kobo, supported or not.
export async function koboSerial(root) {
  const dot = await root.getDirectoryHandle(".kobo");
  return parseVersion(new TextDecoder().decode(await readFile(dot, "version"))).serial;
}

// The whole drive as one zip, written straight to the file the user chose.
export async function writeFullBackup(root, writable, progress) {
  const entries = await listFiles(root);
  let end = 0;
  const sink = {
    async write(bytes) {
      await writable.write({ type: "write", position: end, data: bytes });
      end += bytes.length;
    },
    async patch(position, bytes) {
      await writable.write({ type: "write", position, data: bytes });
    },
  };
  await writeZip(entries, sink, progress);
  return entries.length;
}

export async function readBackup(root) {
  const dot = await root.getDirectoryHandle(".kobo");
  const conf = await (await dot.getDirectoryHandle("Kobo")).getFileHandle("Kobo eReader.conf");
  return buildZip([
    { name: "KoboReader.sqlite", data: await readFile(dot, "KoboReader.sqlite") },
    { name: "Kobo eReader.conf", data: await readBlob(await conf.getFile()) },
  ]);
}

// Package last: the Kobo installs it on eject, and by then everything else has to be in place.
async function saveRecord(root, version, patches) {
  const adds = await root.getDirectoryHandle(".adds", { create: true });
  if (patches.length) {
    await writeFile(adds, RECORD, encoder.encode(JSON.stringify({ firmware: version, patches })));
  } else if (await exists(adds, RECORD)) {
    await adds.removeEntry(RECORD);
  }
  await cleanMacFiles(adds);
}

export async function writeToKobo(root, plan, progress = () => {}) {
  const dot = await root.getDirectoryHandle(".kobo");
  const koboDir = await dot.getDirectoryHandle("Kobo");
  const packageBytes = plan.files.length ? await buildPackage(plan.files) : null;
  if (plan.installsNickelMenu && await dirExists(root, ".adds")) {
    // A leftover from removing it earlier would make the new install remove itself on restart.
    const adds = await root.getDirectoryHandle(".adds");
    if (await dirExists(adds, "nm")) {
      const nm = await adds.getDirectoryHandle("nm");
      if (await exists(nm, "uninstall")) await nm.removeEntry("uninstall");
    }
  }
  if (plan.koreader) await copyKOReader(root, plan.koreader, progress);
  const removing = plan.remove?.koreader || plan.remove?.nickelmenu;
  if (!plan.files.length && !plan.koreader && !plan.settings.length && !removing) {
    throw new Refused("there's nothing to change, so nothing was written");
  }
  if (removing) await removeAddons(root, plan.remove);
  if (plan.settings.length) {
    const before = await readFile(koboDir, "Kobo eReader.conf");
    let text = new TextDecoder("utf-8", { fatal: true }).decode(before);
    for (const [section, key, value] of plan.settings) text = setConf(text, section, key, value);
    await writeFile(koboDir, "Kobo eReader.conf", encoder.encode(text));
  }
  if (plan.record) {
    const { firmware } = parseVersion(new TextDecoder().decode(await readFile(dot, "version")));
    await saveRecord(root, firmware, plan.record);
  }
  if (packageBytes) await writeFile(dot, "KoboRoot.tgz", packageBytes);
  await cleanMacFiles(koboDir);
  await cleanMacFiles(dot);
  if (!packageBytes) return;
  const written = await readFile(dot, "KoboRoot.tgz");
  if (await sha256(written) !== await sha256(packageBytes)) {
    await dot.removeEntry("KoboRoot.tgz");
    throw new Refused("the package didn't read back correctly, so it was removed. Nothing will "
      + "install. Reconnect the Kobo and try again");
  }
}
