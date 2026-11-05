import {
  Refused, UnsupportedFirmware, inspectKobo, koboSerial, listIds, writeFullBackup, planPatch, planUndo, readBackup, readFirmware,
  readDictionary, readKOReader, sha256, writeToKobo,
} from "./patcher.js";
import {
  ALL_SWITCH, CATEGORIES, DICTIONARIES, FIRMWARE, FIRMWARE_LIST_URL, KOREADER, MODEL_NAMES, firmwareRequestUrl, firmwareSearchUrl, registryFor,
} from "./supported.js";

const ADDON_FILES = {
  koreader: { releases: KOREADER, accept: ".zip", read: readKOReader },
};

// Add-ons on the Kobo that this page doesn't offer there, shown ticked so unticking can remove them.
const LEFTOVER_ADDONS = [
  {
    id: "nickelmenu",
    title: "NickelMenu",
    description: "No longer offered here, now that Kobo Patcher puts KOReader on the More page. "
      + "Untick it to remove NickelMenu and any menu entries it shows.",
  },
  {
    id: "koreader",
    title: "KOReader",
    description: "Not offered on this firmware, because Kobo Patcher isn't. Untick it to delete KOReader.",
  },
];

const $ = (id) => document.getElementById(id);
const canWrite = "showDirectoryPicker" in window && "showSaveFilePicker" in window;
const state = {
  drive: null, root: null, kobo: null, items: [], edits: {}, firmware: null, busy: false, addons: {}, mods: {}, list: null, riskAccepted: false, dictionaries: [],
};

function say(id, text, kind = "") {
  $(id).textContent = text;
  $(id).className = `status ${kind}`;
}

function checked(id) {
  return !!$(`patch-${id}`)?.checked;
}

function offered(kind) {
  return state.items.filter((item) => item.kind === kind);
}

function chosenPatches() {
  return offered("patch").filter((patch) => checked(patch.id)).map((patch) => patch.id);
}

function isInstalled(id) {
  return !!state.kobo?.installed[id];
}

// A ticked add-on that's already on the Kobo needs nothing. Unticking an installed one removes it.
function chosenAddons() {
  return offered("addon").filter((addon) => checked(addon.id) && !isInstalled(addon.id));
}

// Only an add-on with a row on the page can be unticked.
function addonsToRemove() {
  const unticked = (id) => isInstalled(id) && !!$(`patch-${id}`) && !checked(id);
  return { nickelmenu: unticked("nickelmenu"), koreader: unticked("koreader") };
}

function modInstalled(id) {
  return state.kobo?.installed.mods[id];
}

// A ticked mod is added if it isn't on the Kobo, or if this site has a newer build than the one there.
function modsToInstall() {
  return offered("mod").filter((mod) => checked(mod.id) && modInstalled(mod.id) !== state.mods[mod.id].sha256)
    .map((mod) => ({ id: mod.id, ...state.mods[mod.id] }));
}

function modsToRemove() {
  return offered("mod").filter((mod) => modInstalled(mod.id) && !checked(mod.id)).map((mod) => mod.id);
}

function recorded() {
  return state.kobo?.installed.patches ?? [];
}

// With Kobo Patcher ticked, the patches in its list are switched on the Kobo, not put in the files.
function viaKoboPatcher() {
  return !!state.list && checked("patches");
}

function forFiles(ids) {
  return ids.filter((id) => !(viaKoboPatcher() && state.list.ids.has(id)));
}

// The firmware is only needed to change which patches are in the Kobo's system files.
function patchesChanged() {
  const chosen = forFiles(chosenPatches());
  const before = forFiles(recorded());
  return chosen.length !== before.length || chosen.some((id) => !before.includes(id));
}

// What a patch in Kobo Patcher's list is now: as switched on the Kobo, or else as the files have it.
function onNow(id) {
  return state.kobo?.installed.choices[id] ?? recorded().includes(id);
}

function choicesChanged() {
  if (!viaKoboPatcher()) return false;
  return state.kobo.installed.list !== state.list.text || [...state.list.ids].some((id) => checked(id) !== onNow(id));
}

function readyToPatch() {
  const addons = chosenAddons();
  const { nickelmenu, koreader } = addonsToRemove();
  const modsChanged = modsToInstall().length || modsToRemove().length;
  const dictionaries = state.dictionaries.length;
  if (!patchesChanged() && !choicesChanged() && !addons.length && !nickelmenu && !koreader && !modsChanged && !dictionaries) return false;
  return addons.every((addon) => state.addons[addon.id]) && (!patchesChanged() || !!state.firmware);
}

function render() {
  $("browser-notice").hidden = canWrite;
  $("step-firmware").hidden = !state.kobo || (!patchesChanged() && !forFiles(recorded()).length);
  $("step-patches").classList.toggle("locked", !state.kobo);
  $("step-firmware").classList.toggle("locked", !state.kobo);
  // Stays readable after a run, when the Kobo is no longer selected, for its "Done" message.
  $("step-run").classList.toggle("locked", !state.kobo && !$("run-status").textContent);
  let step = 0;
  for (const number of document.querySelectorAll("main > section:not([hidden]) h2 > span")) {
    step += 1;
    number.textContent = step;
  }
  $("connect").disabled = !canWrite || state.busy;
  $("backup").disabled = !state.drive || state.busy;
  $("firmware").disabled = state.busy || !state.kobo;
  $("dictionary-files").disabled = state.busy || !state.kobo;
  for (const box of document.querySelectorAll("#patches input")) box.disabled = state.busy;
  showAllSwitches();
  if ($("via-kobopatcher")) $("via-kobopatcher").hidden = !viaKoboPatcher();
  // KOReader starts from the row Kobo Patcher adds to More, so adding it brings Kobo Patcher along.
  const addingKOReader = !!$("addon-koreader") && checked("koreader") && !isInstalled("koreader");
  if ($("patch-patches")) {
    if (addingKOReader) $("patch-patches").checked = true;
    $("patch-patches").disabled = state.busy || addingKOReader;
  }
  for (const addon of offered("addon")) {
    $(`addon-${addon.id}`).hidden = !checked(addon.id) || isInstalled(addon.id);
  }
  const installed = state.kobo?.installed;
  const canUndo = !!state.firmware || installed?.nickelmenu || installed?.koreader || Object.keys(installed?.mods ?? {}).length > 0;
  $("patch").disabled = !state.kobo || state.busy || !readyToPatch();
  $("undo").disabled = !state.kobo || state.busy || !canUndo;
}

function explain(error) {
  if (error instanceof Refused) return error.message[0].toUpperCase() + error.message.slice(1);
  if (error.name === "NotAllowedError") return "The browser wasn't given access, so nothing was changed.";
  return `Something went wrong: ${error.message}`;
}

async function busy(work) {
  state.busy = true;
  render();
  try {
    await work();
  } finally {
    state.busy = false;
    render();
  }
}

function clearFirmware() {
  state.firmware = null;
  $("firmware").value = "";
  say("firmware-status", "");
  say("run-status", "");
}

function showFirmwareLinks() {
  const { kobo } = state;
  const link = (href, textContent) => element("a", { href, textContent, target: "_blank", rel: "noopener" });
  $("firmware-links").replaceChildren(...(kobo ? [
    `Changing patches needs firmware ${kobo.firmware}. `,
    link(kobo.build.url, `Download it from Kobo (${kobo.build.sizeMB} MB)`),
    ". If that link stops working, ",
    link(firmwareSearchUrl(kobo.name, kobo.firmware), "search Google"),
    " or ",
    link(FIRMWARE_LIST_URL, "check the firmware list"),
    ".",
  ] : []));
}

async function chooseFirmware(file) {
  clearFirmware();
  say("firmware-status", `Reading ${file.name}…`);
  await busy(async () => {
    try {
      const firmware = await readFirmware(file);
      if (await sha256(firmware.libnickel) !== state.kobo.build.nickelSha256
        || await sha256(firmware.nickel) !== state.kobo.build.nickelBinSha256) {
        throw new Refused(`this isn't firmware ${state.kobo.firmware} for the ${state.kobo.name}`);
      }
      state.firmware = firmware;
      say("firmware-status", `Firmware ${state.kobo.firmware}, recognised and unchanged.`, "good");
    } catch (error) {
      say("firmware-status", explain(error), "bad");
    }
  });
}

function element(tag, props = {}, ...children) {
  const node = Object.assign(document.createElement(tag), props);
  node.append(...children);
  return node;
}

// Ticking a patch unticks the others in its group.
function tick(item, on) {
  $(`patch-${item.id}`).checked = on;
  if (!on || !item.group) return;
  for (const other of offered("patch")) {
    if (other.group === item.group && other.id !== item.id) $(`patch-${other.id}`).checked = false;
  }
}

// "All <category>" is ticked when every patch in it is, and half ticked when some are.
function showAllSwitches() {
  for (const category of ALL_SWITCH) {
    const box = $(`all-${slug(category)}`);
    if (!box) continue;
    const on = state.items.filter((item) => item.category === category).map((item) => checked(item.id));
    box.checked = on.every(Boolean);
    box.indeterminate = !box.checked && on.some(Boolean);
  }
}

function allRow(category, items) {
  const box = element("input", { type: "checkbox", id: `all-${slug(category)}` });
  box.addEventListener("change", () => {
    for (const item of items) tick(item, box.checked);
    render();
  });
  const text = element("span", {}, element("strong", { textContent: `All ${category.toLowerCase()}` }),
    element("small", { textContent: "Turns every one below on or off." }));
  return element("label", { className: "patch" }, box, text);
}

function slug(text) {
  return text.toLowerCase().replace(/[^a-z0-9]+/g, "-");
}

function patchRow(item, isChecked) {
  const box = element("input", { type: "checkbox", id: `patch-${item.id}`, checked: isChecked });
  box.addEventListener("change", () => {
    tick(item, box.checked);
    render();
  });
  const title = element("strong", { textContent: item.title });
  if (item.tag) title.append(" ", element("em", { className: "tag", textContent: item.tag }));
  const text = element("span", {}, title, element("small", { textContent: item.description }));
  if (item.warning) text.append(element("small", { className: "warning", textContent: item.warning }));
  return element("label", { className: "patch" }, box, text);
}

function addonFile(addon) {
  const { releases, accept, read } = ADDON_FILES[addon.id];
  const [latest] = releases;
  const status = element("p", { className: "status", id: `addon-status-${addon.id}` });
  const input = element("input", { type: "file", accept, ariaLabel: `${addon.title} file` });
  input.addEventListener("change", async () => {
    const [file] = input.files;
    if (!file) return;
    delete state.addons[addon.id];
    say(status.id, "Checking…");
    await busy(async () => {
      try {
        state.addons[addon.id] = await read(file);
        say(status.id, `${addon.title} ${state.addons[addon.id].version}, recognised.`, "good");
      } catch (error) {
        say(status.id, explain(error), "bad");
      }
    });
  });
  const download = element("a", {
    href: latest.url, textContent: `Download ${addon.title} ${latest.version}`, rel: "noopener",
  });
  return element("div", { className: "addon", id: `addon-${addon.id}`, hidden: true },
    element("p", {}, download, " from its official page, then choose the file here:"), input, status);
}

function showPatches() {
  // Ticked if already on the Kobo, because changing patches rebuilds them all from the originals.
  // With Kobo Patcher, its patches are ticked as it has them.
  const rows = [element("p", {
    id: "via-kobopatcher", className: "status", hidden: true,
    textContent: "Kobo Patcher switches the patches it has on the Kobo itself, so they don't need the firmware file. "
      + "You can change them there too, under Settings.",
  })];
  const mine = (id) => !!modInstalled("patches") && !!state.list?.ids.has(id);
  for (const category of CATEGORIES) {
    const items = state.items.filter((item) => item.category === category);
    if (items.length) rows.push(element("h3", { textContent: category }));
    if (items.length > 1 && ALL_SWITCH.includes(category)) rows.push(allRow(category, items));
    for (const item of items) {
      if (item.kind === "addon") rows.push(patchRow(item, isInstalled(item.id)), addonFile(item));
      else if (item.kind === "mod") rows.push(patchRow(item, !!modInstalled(item.id)));
      else rows.push(patchRow(item, mine(item.id) ? onNow(item.id) : recorded().includes(item.id)));
    }
  }
  const leftovers = LEFTOVER_ADDONS.filter((addon) => isInstalled(addon.id) && !state.items.some((item) => item.id === addon.id));
  if (leftovers.length && !offered("addon").length) rows.push(element("h3", { textContent: "Add-ons" }));
  for (const addon of leftovers) rows.push(patchRow(addon, true));
  $("patches").replaceChildren(...rows);
  showAllSwitches();
}

function versionList(items) {
  return element("ul", {}, ...items.map((text) => element("li", { textContent: text })));
}

function showSupported() {
  const devices = FIRMWARE.flatMap((build) => [
    element("h3", { textContent: `${MODEL_NAMES[build.model]}, firmware ${build.firmware}` }),
    versionList(registryFor(build.model, build.firmware).map((item) => item.title)),
  ]);
  $("supported-list").replaceChildren(
    element("p", { textContent: "Your Kobo's model and firmware have to match a heading here exactly. "
      + "Find your firmware under More > Settings > Device information." }),
    ...devices,
    element("h3", { textContent: "Add-on files" }),
    versionList([
      ...KOREADER.map((release) => `KOReader ${release.version}`),
    ]),
    element("p", { textContent: "The page checks the two system files it uses from a firmware zip against "
      + "known hashes and ignores the rest, so a copy from another site is refused if they differ." }),
    element("p", {}, "Older firmware is in the ", element("a", {
      href: FIRMWARE_LIST_URL, target: "_blank", rel: "noopener", textContent: "community firmware list",
    }), "."),
  );
  $("supported").showModal();
}

function describe(kobo) {
  const extras = [kobo.installed.nickelmenu && "NickelMenu", kobo.installed.koreader && "KOReader"].filter(Boolean);
  const count = kobo.installed.patches.length;
  if (count) extras.push(`${count} ${count === 1 ? "patch" : "patches"} from this page`);
  const mods = offered("mod").filter((mod) => modInstalled(mod.id));
  extras.push(...mods.map((mod) => `the ${mod.id} mod`));
  const already = extras.length ? ` It already has ${new Intl.ListFormat("en").format(extras)}.` : "";
  const newer = mods.filter((mod) => modInstalled(mod.id) !== state.mods[mod.id].sha256)
    .map((mod) => ` A newer ${mod.id} mod is ready, and pressing Patch installs it.`).join("");
  return `Found a ${kobo.name} on ${kobo.firmware}.${already}${newer}`;
}

async function selectKobo() {
  let root;
  try {
    root = await window.showDirectoryPicker({ id: "kobo", mode: "readwrite" });
  } catch (error) {
    if (error.name !== "AbortError") say("kobo-status", explain(error), "bad");
    return;
  }
  Object.assign(state, { drive: null, root: null, kobo: null, items: [], edits: {}, addons: {}, mods: {}, list: null });
  say("backup-status", "");
  $("patches").replaceChildren();
  clearFirmware();
  showFirmwareLinks();
  await busy(async () => {
    try {
      // A backup doesn't change anything, so it's offered even on a Kobo the patches don't support.
      state.drive = { root, serial: await koboSerial(root) };
    } catch {
      // inspectKobo below explains what's wrong with this folder.
    }
    try {
      const kobo = await inspectKobo(root);
      const response = await fetch(new URL(`patches/${kobo.model}-${kobo.firmware}.json`, import.meta.url), { cache: "no-cache" });
      if (!response.ok) throw new Refused("this page couldn't load its patch list. Reload and try again");
      const edits = await response.json();
      const items = registryFor(kobo.model, kobo.firmware);
      const mods = await loadMods(items.filter((item) => item.kind === "mod"));
      const list = mods.patches ? await loadList(`${kobo.model}-${kobo.firmware}.txt`) : null;
      if (!list) delete mods.patches;
      // KOReader needs Kobo Patcher's row in More, so it goes when Kobo Patcher isn't offered.
      const kept = items.filter((item) => (item.kind !== "mod" || mods[item.id]) && (item.id !== "koreader" || mods.patches));
      Object.assign(state, { root, kobo, edits, mods, list, items: kept });
      showFirmwareLinks();
      showPatches();
      say("kobo-status", describe(kobo), "good");
    } catch (error) {
      say("kobo-status", explain(error), "bad");
      if (error instanceof UnsupportedFirmware) {
        $("kobo-status").append(" or ", element("a", {
          href: firmwareRequestUrl(error.model, error.firmware), target: "_blank", rel: "noopener", textContent: "ask for it to be added",
        }), ".");
      }
    }
  });
}

// Built by `make mods`. A mod this copy of the page doesn't have isn't offered.
async function loadMods(items) {
  const mods = {};
  for (const item of items) {
    try {
      const response = await fetch(new URL(item.file, import.meta.url), { cache: "no-cache" });
      if (!response.ok) throw new Error(`${response.status}`);
      const data = new Uint8Array(await response.arrayBuffer());
      mods[item.id] = { data, sha256: await sha256(data) };
    } catch (error) {
      console.warn(`not offering the ${item.id} mod, because ${item.file} didn't load: ${error.message}`);
    }
  }
  return mods;
}

// Kobo Patcher's patch list for this firmware, also built by `make mods`. Without it there's no mod.
async function loadList(name) {
  try {
    // Format 2, which the Kobo Patcher this page installs reads.
    const response = await fetch(new URL(`mods/lists/2/${name}`, import.meta.url), { cache: "no-cache" });
    if (!response.ok) throw new Error(`${response.status}`);
    const text = await response.text();
    return { name, text, ids: new Set(listIds(text)) };
  } catch (error) {
    console.warn(`not offering Kobo Patcher, because its list ${name} didn't load: ${error.message}`);
    return null;
  }
}

async function backUpEverything() {
  let target;
  try {
    target = await window.showSaveFilePicker({
      id: "kobo-backup",
      suggestedName: `${state.drive.serial}-full-backup.zip`,
      types: [{ description: "Zip file", accept: { "application/zip": [".zip"] } }],
    });
  } catch (error) {
    if (error.name !== "AbortError") say("backup-status", explain(error), "bad");
    return;
  }
  await busy(async () => {
    let writable;
    try {
      say("backup-status", "Listing files…");
      writable = await target.createWritable();
      const count = await writeFullBackup(state.drive.root, writable, ({ done, total, bytes }) => {
        say("backup-status", `Backing up: ${done} of ${total} files, ${Math.round(bytes / 1e6)} MB…`);
      });
      say("backup-status", "Finishing the zip…");
      await writable.close();
      say("backup-status", `Backed up ${count} files to ${target.name}, in the folder you chose. It can contain sign-in tokens, so keep it private.`, "good");
    } catch (error) {
      await writable?.abort().catch(() => {});
      say("backup-status", `The backup didn't finish, so no zip was saved. ${explain(error)}`, "bad");
    }
  });
}

async function run(makePlan) {
  let backupFile;
  try {
    backupFile = await window.showSaveFilePicker({
      id: "kobo-backup",
      suggestedName: `${state.kobo.serial}-backup.zip`,
      types: [{ description: "Zip file", accept: { "application/zip": [".zip"] } }],
    });
  } catch (error) {
    if (error.name !== "AbortError") say("run-status", explain(error), "bad");
    return;
  }
  await busy(async () => {
    try {
      // Check again in case a different Kobo was plugged in since it was selected.
      const now = await inspectKobo(state.root);
      if (now.serial !== state.kobo.serial || now.firmware !== state.kobo.firmware) {
        throw new Refused("this isn't the Kobo you selected. Select it again");
      }
      const plan = makePlan();

      say("run-status", "Saving the backup…");
      const backup = await backupFile.createWritable();
      await backup.write(await readBackup(state.root));
      await backup.close();

      say("run-status", "Writing to the Kobo…");
      await writeToKobo(state.root, plan, (text) => say("run-status", text));
      Object.assign(state, { drive: null, root: null, kobo: null, items: [], edits: {}, firmware: null, addons: {}, mods: {}, list: null, dictionaries: [] });
      $("dictionary-files").value = "";
      say("dictionary-status", "");
      $("patches").replaceChildren();
      showFirmwareLinks();
      say("kobo-status", "");
      say("firmware-status", "");
      const after = plan.files.length
        ? "It installs and restarts by itself. If it doesn't, power it off and on."
        : plan.storage.length
          ? "Then open Settings > Kobo Patcher on it, and the patches switch straight away."
          : "Then restart it.";
      say("run-status", `Done. Eject the Kobo properly before unplugging it. ${after} `
        + "The backup can contain sign-in tokens, so keep it private.", "good");
    } catch (error) {
      say("run-status", explain(error), "bad");
    }
  });
}

function patch() {
  const addons = chosenAddons().map((addon) => addon.id);
  const changed = patchesChanged();
  const mod = viaKoboPatcher() ? {
    list: state.list,
    choices: Object.fromEntries([...state.list.ids].map((id) => [id, checked(id)])),
  } : null;
  run(() => planPatch(changed ? state.firmware : null, changed ? forFiles(chosenPatches()) : [], {
    edits: state.edits,
    remove: { ...addonsToRemove(), mods: modsToRemove() },
    mods: modsToInstall(),
    mod,
    koreader: addons.includes("koreader") ? state.addons.koreader : null,
    dictionaries: state.dictionaries,
    on: chosenPatches(),
  }));
}

function undo() {
  const { installed } = state.kobo;
  const lines = state.firmware ? ["Put Kobo's original system files back, which removes all patches."] : [];
  if (installed.koreader) lines.push("Delete KOReader, including its settings in .adds/koreader.");
  if (installed.nickelmenu) lines.push("Remove NickelMenu and any custom menu entries it shows.");
  for (const id of Object.keys(installed.mods)) lines.push(`Remove the ${id} mod the next time the Kobo restarts.`);
  if (!state.firmware && recorded().length) {
    lines.push("Leave your patches in place. To remove those too, cancel and choose the firmware file first.");
  }
  if (window.confirm(`This will:\n\n- ${lines.join("\n- ")}\n\nContinue?`)) {
    run(() => planUndo(state.firmware, installed));
  }
}

// Files dropped on a drop zone go to choose, once a Kobo is selected.
function dropZone(zone, choose) {
  for (const name of ["dragenter", "dragover"]) {
    zone.addEventListener(name, (event) => {
      event.preventDefault();
      zone.classList.add("over");
    });
  }
  for (const name of ["dragleave", "drop"]) {
    zone.addEventListener(name, (event) => {
      event.preventDefault();
      zone.classList.remove("over");
    });
  }
  zone.addEventListener("drop", (event) => {
    const files = [...event.dataTransfer.files];
    if (files.length && !state.busy && state.kobo) choose(files);
  });
}
dropZone($("drop"), ([file]) => chooseFirmware(file));
dropZone($("dictionary-drop"), chooseDictionaries);
$("firmware").addEventListener("change", (event) => {
  const [file] = event.target.files;
  if (file) chooseFirmware(file);
});
$("help").addEventListener("click", showSupported);
$("connect").addEventListener("click", () => {
  if (state.riskAccepted) selectKobo();
  else $("risk").showModal();
});
$("risk-cancel").addEventListener("click", () => $("risk").close());
// The folder picker needs a click to open from, so it starts from this one.
$("risk-accept").addEventListener("click", () => {
  state.riskAccepted = true;
  $("risk").close();
  selectKobo();
});
// Dictionaries need the patch that lets Kobo use one for a language it doesn't list, so choosing
// one ticks it, where this firmware has it.
async function chooseDictionaries(files) {
  state.dictionaries = [];
  say("dictionary-status", files.length ? "Checking…" : "");
  await busy(async () => {
    const lines = [];
    for (const file of files) {
      try {
        const dictionary = await readDictionary(file);
        state.dictionaries.push(dictionary);
        lines.push(`${dictionary.name}: recognised.`);
      } catch (error) {
        lines.push(explain(error));
      }
    }
    const patchBox = $("patch-book-language-dictionaries");
    if (state.dictionaries.length && patchBox && !patchBox.checked) {
      patchBox.checked = true;
      lines.push("Dictionaries for every book language is ticked too, so Kobo uses them.");
    }
    const allGood = state.dictionaries.length === files.length;
    say("dictionary-status", lines.join(" "), allGood ? "good" : "bad");
  });
  render();
}
$("dictionary-files").addEventListener("change", (event) => chooseDictionaries([...event.target.files]));

// The download link follows the language chosen, starting at the browser's own where there's one.
function showDictionaryLanguages() {
  const names = new Intl.DisplayNames(["en"], { type: "language" });
  const languages = DICTIONARIES.languages.map((code) => ({ code, name: names.of(code) ?? code }))
    .sort((a, b) => a.name.localeCompare(b.name));
  const select = $("dictionary-language");
  select.replaceChildren(...languages.map(({ code, name }) => element("option", { value: code, textContent: name })));
  const own = navigator.languages.map((tag) => tag.split("-")[0].toLowerCase()).find((code) => DICTIONARIES.languages.includes(code));
  select.value = own ?? languages[0].code;
  const link = () => {
    $("dictionary-download").href = DICTIONARIES.file(select.value);
    $("dictionary-download").textContent = `Download ${names.of(select.value) ?? select.value}`;
  };
  select.addEventListener("change", link);
  link();
}
showDictionaryLanguages();
$("backup").addEventListener("click", backUpEverything);
$("patch").addEventListener("click", patch);
$("undo").addEventListener("click", undo);
render();
