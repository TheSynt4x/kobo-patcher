// What the patcher accepts. The page refuses anything not listed here, and its
// "supported versions" window is built from this file. Firmware and compiled patches
// come from patches.yaml, through `make patches`.
import { COMPILED_PATCHES, FIRMWARE, PATCH_CATEGORIES } from "./patches/compiled.js";

export { FIRMWARE };

// The page's sections, in order. Patch categories are set in patches.yaml.
export const CATEGORIES = ["Mods", ...PATCH_CATEGORIES, "Add-ons"];

// Names for the model codes in .kobo/version.
export const MODEL_NAMES = Object.fromEntries(FIRMWARE.map(({ model, name }) => [model, name]));

export const FIRMWARE_LIST_URL = "https://pgaskin.net/KoboStuff/kobofirmware.html";

// Opens the request form with this Kobo's details filled in. The serial isn't included.
export function firmwareRequestUrl(model, firmware) {
  const query = new URLSearchParams({
    template: "firmware.yml", title: `Support ${model} on ${firmware}`, model, firmware,
  });
  return `https://github.com/TheSynt4x/kobo-patcher/issues/new?${query}`;
}

export function firmwareSearchUrl(name, firmware) {
  const query = `${name} firmware ${firmware} download kobo-update`;
  return `https://www.google.com/search?q=${encodeURIComponent(query)}`;
}

export const KOREADER = [
  {
    version: "2026.07.1",
    sha256: "0f36a62ce73b12516f969e4ad7862cc06920afb03c7bd4db29a0d990bdd84b2f",
    url: "https://github.com/koreader/koreader/releases/download/v2026.07.1/koreader-kobo-v2026.07.1.zip",
  },
];

const DRIVE_FIRMWARE = {};
const DROPBOX_FIRMWARE = {};
// A mod is only offered where its build has run on a real Kobo, marked mod: true in patches.yaml.
// The mod updates itself on the same firmware. KOReader is started from its row in More, so it's
// offered on the same firmware too.
const PATCHES_MOD_FIRMWARE = {};
for (const { model, firmware, google_drive: drive, dropbox, mod } of FIRMWARE) {
  if (drive) (DRIVE_FIRMWARE[model] ??= []).push(firmware);
  if (dropbox) (DROPBOX_FIRMWARE[model] ??= []).push(firmware);
  if (mod) (PATCHES_MOD_FIRMWARE[model] ??= []).push(firmware);
}

// Every patch, mod and add-on, and the model and firmware versions it's offered on.
// "patch" items change Kobo's system files. "mod" items add a library this site builds, from
// mods/ in its source. "addon" items install from their own release file.
const categoryOrder = (item) => CATEGORIES.indexOf(item.category);

export const REGISTRY = [
  {
    id: "patches",
    kind: "mod",
    category: "Mods",
    title: "Kobo Patcher",
    tag: "Experimental",
    description: "Adds Kobo Patcher to the top of Settings, to switch these patches on and off on the Kobo itself, straight "
      + "away. With it, patches from this page don't need the firmware file, and it can check for new patches over Wi-Fi.",
    warning: "Tested on one Kobo Nia. Use it at your own risk.",
    file: "mods/libpatches.so",
    targets: PATCHES_MOD_FIRMWARE,
  },
  {
    id: "google-drive",
    kind: "patch",
    category: "Cloud storage",
    title: "Google Drive in the More menu",
    description: "Turns off the device-model check that hides Google Drive, and adds the Drive settings.",
    needsFirmware: true,
    targets: DRIVE_FIRMWARE,
  },
  {
    id: "dropbox",
    kind: "patch",
    category: "Cloud storage",
    title: "Dropbox in the More menu",
    description: "Turns off the device-model check that hides Dropbox, and adds the Dropbox settings.",
    needsFirmware: true,
    targets: DROPBOX_FIRMWARE,
  },
  ...COMPILED_PATCHES,
  {
    id: "koreader",
    kind: "addon",
    category: "Add-ons",
    title: "KOReader",
    description: "Copies KOReader to the Kobo, and Kobo Patcher adds it to the More page. Needs Kobo Patcher, and takes a few minutes.",
    targets: PATCHES_MOD_FIRMWARE,
  },
].sort((a, b) => categoryOrder(a) - categoryOrder(b));

export function registryFor(model, firmware) {
  return REGISTRY.filter((item) => item.targets[model]?.includes(firmware));
}
