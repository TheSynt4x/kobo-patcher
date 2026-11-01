// What the patcher accepts. The page refuses anything not listed here, and its
// "supported versions" window is built from this file. Firmware and compiled patches
// come from patches.yaml, through `make patches`.
import { COMPILED_PATCHES, FIRMWARE } from "./patches/compiled.js";

export { FIRMWARE };

// Names for the model codes in .kobo/version, so an unsupported Kobo can still be named.
export const MODEL_NAMES = { N306: "Kobo Nia", N428: "Kobo Libra Colour" };

export const FIRMWARE_LIST_URL = "https://pgaskin.net/KoboStuff/kobofirmware.html";

export function firmwareSearchUrl(name, firmware) {
  const query = `${name} firmware ${firmware} download kobo-update`;
  return `https://www.google.com/search?q=${encodeURIComponent(query)}`;
}

export const NICKELMENU = [
  {
    version: "0.6.0",
    sha256: "322ff9aa863860e8f5f7e0b55cae561c54bf95983b9bce1d19819d1225d064af",
    url: "https://github.com/pgaskin/NickelMenu/releases/download/v0.6.0/KoboRoot.tgz",
  },
];

export const KOREADER = [
  {
    version: "2026.07.1",
    sha256: "0f36a62ce73b12516f969e4ad7862cc06920afb03c7bd4db29a0d990bdd84b2f",
    url: "https://github.com/koreader/koreader/releases/download/v2026.07.1/koreader-kobo-v2026.07.1.zip",
  },
];

// Add-ons don't touch system files, so they're offered on all firmware we know.
const ALL_FIRMWARE = {};
for (const { model, firmware } of FIRMWARE) (ALL_FIRMWARE[model] ??= []).push(firmware);

// Every patch and add-on, and the model and firmware versions it's offered on.
// "patch" items change Kobo's system files. "addon" items install from their own release file.
export const REGISTRY = [
  {
    id: "google-drive",
    kind: "patch",
    title: "Google Drive in the More menu",
    description: "Turns off the device-model check that hides Google Drive, and adds the Drive settings.",
    needsFirmware: true,
    targets: { N306: ["4.38.23852"] },
  },
  ...COMPILED_PATCHES,
  {
    id: "nickelmenu",
    kind: "addon",
    title: "NickelMenu",
    description: "Adds a menu button to the bottom bar of the home screen.",
    targets: ALL_FIRMWARE,
  },
  {
    id: "koreader",
    kind: "addon",
    title: "KOReader",
    description: "Copies KOReader to the Kobo and adds it to the NickelMenu button. Needs NickelMenu, and takes a few minutes.",
    targets: ALL_FIRMWARE,
  },
];

export function registryFor(model, firmware) {
  return REGISTRY.filter((item) => item.targets[model]?.includes(firmware));
}
