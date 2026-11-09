# Mods

Mods add to Kobo's UI using Kobo's own widgets, so a new row or screen looks like it was always there.
A mod is Lua scripts on top of `kit/`, and the kit's Lua API is what's documented and kept working. Every call is in [LUA.md](LUA.md), with the rules a mod keeps to and what has been tried. `example/` is a Lua mod that shows nearly everything the kit can do, a page for each part, with notes: build it with `build.sh example` and open Example on the More page. The kit is C++, built against Qt 5.2.1's headers, the Qt that Nickel runs, and uses Nickel's own Qt when it runs. There's no Qt to install: the build fetches the headers, and Lua and Monocypher too (`kit/vendor.sh`), checked against their releases' SHA-256, into `build/`. The kit's own C layer, `kit/nk.h`, is internal: C++ mods can use it, but it may change in any release.

```lua
-- main.lua: a row in Kobo's Settings, opening a screen of Kobo's own rows.
nk.settings{name = "Hello", build = function(page)
  page:section("HELLO")
  page:value("Greeting", "Hi")
  page:toggle("Be friendly", true, function(on) nk.log("friendly: " .. tostring(on)) end)
  page:button("Say it", "Start", function() nk.toast("Hello", "Hi there") end)
end}
```

Build it with `mods/build.sh <folder>`, which needs zig 0.17.0, curl the first time (for Qt's headers, which `kit/qt-headers.sh` fetches into `build/qt` and checks), and Python with PyYAML for `patches`. It writes
`build/lib<folder>.so`, which goes in `/usr/local/Kobo/imageformats/` on the Kobo. A folder elsewhere builds the same way, with `build.sh path/to/folder`, and its `.so` goes in `build/` beside that folder. A `plugin` file in the folder gives it its own Qt plugin class and key, 15 and 4 characters, so Qt loads it beside other mods.

**Status: works on a real Kobo Nia on firmware 4.38.23852 and a real Kobo Libra Colour on 4.46.23852, one device each. Not yet tried on other models. The crash protection below has been tested on the Nia for a crash while a mod starts, not yet for one while a screen opens. The missing-symbol check has only been tested in an emulator.**

## What keeps a Kobo safe

- Every symbol the mod uses is checked when Nickel loads it, before the mod does anything. The build lists them: what the `.so` leaves undefined, and the names the kit looks up. If one is missing, the mod does nothing and there's no crash.
- Before a screen opens, the controller's vtable is compared with what's expected.
- If Nickel crashes or freezes while a mod starts or opens a screen, or soon after `nk_failsafe`, the mod is moved to `/usr/local/Kobo/<name>.disabled` and isn't loaded again. It's only put back once Nickel's main thread has kept running for a while, so a freeze counts too. The Kobo may freeze or restart first. If it's stuck, hold power for about 15 seconds and turn it on again. Nickel exiting normally, `nk_reboot`, or a command from `nk_spawn` stopping Nickel, doesn't count as a crash.
- After an update from `nk.updates`, a mod with a version keeps the build it replaced as `/usr/local/Kobo/<name>.previous`, and the new one is on probation for its first 3 starts, noted in `<name>.probation`. If the guard catches it crashing or freezing then, as it starts or as one of its screens opens, the build before it is what loads next time, not nothing: while the guard is on, that build is linked in where Qt loads from, and the new one is linked aside, so `lib<name>.so` is always a whole build on disk. The build that comes back says so once Home is up, and that version isn't offered again on that Kobo. After 3 clean starts, the build before it is let go. While one mod has a build on probation, the others leave their startup guards off, so a crash as Nickel starts sets aside only the new build, not every mod that was starting. A mod installed another way, with no build before it, is set aside as above.
- Nothing a mod runs while it loads may decode an image or use Qt's plugins: that's `main.lua`'s top level, and the kit calls made there. Mods are Qt image plugins, loaded while Qt's plugin loader holds its lock, so decoding a PNG then waits on that lock for ever, and Nickel never starts. Kit calls like `nk.tab` only record what they're asked for, and do the rest when Kobo builds its screens.
- `/mnt/onboard/.adds/<name>/uninstall` removes the mod the next time Nickel starts.

A crash after a screen has opened isn't caught.

## Packs: your own mods and patches, added on the page

A pack is a zip of your own mods and patches, which anyone can add to their Kobo on the page, under "Your own mods and patches". `tools/pack.py` makes one:

```sh
python3 tools/pack.py mine.zip --id mine --name "My stuff" --author me --mod path/to/mod --patches my-patches.yaml
```

`--mod` is a mod's folder, built with `build.sh`, or a `.so` already built, and can be given more than once. `--patches` is a `patches.yaml` with only a `patches:` list, in this repo's format. Its patches are compiled for this repo's firmware, named `<pack>.<id>`, and shown in Kobo Patcher under the pack's name. The zip's `kobo-pack.txt` lists every file with its SHA-256 and size; `tools/pack.py` describes it.

The page checks a pack whole before offering it, and only uses what's for the Kobo that's connected. Its mods go in as the page's own do, under `imageformats`. Its patches go in `.adds/patches/lists/custom/<pack>-<model>-<firmware>.txt`, which Kobo Patcher reads beside its own list, with the same checks, so a pack's patches need Kobo Patcher. A pack can't use the page's own ids, and its patches have to be named after it. The page's record notes what came from each pack, so unticking it, or Undo, takes it all off. A newer zip replaces the older one. Packs aren't signed: the page says to add only ones from people you trust.

## Developing on a Kobo

`devtools/` is a mod for developing on a real Kobo, driven from the computer over Wi-Fi with `tools/kobo`: tap and swipe, what's on screen with its text and places, screenshots, every mod's log and Nickel's crashes, files and a shell, pushing a Lua mod's changed scripts without a rebuild, and running whole scenarios of steps. Anyone with its token is root on the Kobo, so it's only for your own Kobo on your own Wi-Fi. See [devtools/README.md](devtools/README.md).

### Dev builds and releases

Only the site's deploy builds a release, with `KOBO_RELEASE=1`, from committed code. Every other build is a dev build, and says so: `build.sh` puts `dev`, the commit, `+` for changes not committed, and the time in the `.so`, and the kit shows DEV BUILD at the end of each of that mod's rows on the More page. Kobo Patcher shows it under THIS KOBO too, and Check offers a dev build the site's release of the same version, as the way back. A release is the same bytes each time it's built, and the deploy refuses a `libpatches.so` that changed under a version the site already has, so a version is always one build.

From a change to a release:

1. In the emulator: Nickel starts, every mod's guard clears, and the change shows and works.
2. On your own Kobo, with `tools/kobo install` or `push`: its More page says DEV BUILD.
3. Raise the number in `patches/version` if Kobo Patcher changed, and merge. The deploy builds, signs and publishes it.
4. Check on each Kobo, or `tools/kobo reset` on one with devtools. `tools/kobo status` then says `release N, the site's`.

## The Patches mod

It's all Lua, in `patches/`. It adds "Kobo Patcher" to the More page, below Kobo's own rows, and switches patches on and off in Nickel's memory, straight away, and never changes the firmware's files. Each patch's edits are found in libnickel or the nickel program as Nickel has loaded them, and every edit's bytes are checked before any are written. Bytes that match neither side of a patch show as "Changed" and are left alone. A patch with some edits on and some off, as an older version of it leaves the files, shows as off, and switching it finishes or undoes it.

The patch list is a file, `/mnt/onboard/.adds/patches/lists/<model>-<firmware>.txt`, written by `tools/write_lists.py`. It's read when Nickel starts and again whenever Kobo Patcher opens and the file has changed, so new patches arrive without reinstalling the mod. Every line is checked, and edits may only touch libnickel and the nickel program. A patch can have settings in `Kobo eReader.conf` too, which Kobo Patcher writes with `nk.device.set_setting` when the patch is switched there: their values going on, and their `when_off` values going off. That goes through Kobo's own settings, so Nickel follows straight away, but never as the mod loads: choices made again as Nickel starts leave the settings to the file, where the page or an earlier switch put them. Kobo's sync writes some settings from what the store sends, so a patch shouldn't rely on one that sync sets: Google Drive and Dropbox show from code alone, and their patches keep sync from replacing the link address they write.

Its screen is short: this Kobo, with "New patches and Kobo Patcher", how many patches are on, with Edit opening the page of them all by category, and the rest the mod adds, which is switched there and not on Kobo's own screens: the keyboards to switch between with a key beside the space bar, and an entry for each language of your books in My Books' filter menu. Kobo's "Language and dictionaries" page is left as Kobo has it.

"New patches and Kobo Patcher" checks for updates over Wi-Fi, through Qt's own network code. It's the kit's `nk.updates`, as any mod can have (see LUA.md, "A mod's own updates"). It downloads `mods/manifest.txt` from the site, which `tools/sign_mods.mjs` signs with an Ed25519 key, and refuses it unless the signature matches the key Kobo Patcher gives `nk.updates` in `patches/main.lua` ([Monocypher](https://monocypher.org) checks it, through `nk.crypto.ed25519_verify`). Every other file has to match the SHA-512 and size the manifest gives it. This firmware's patch list is one of Kobo Patcher's files to `nk.updates`, so a new one is saved straight away. A new mod is only offered on the firmware the manifest names, the ones marked `mod: true` in `patches.yaml`, and only when its version in the manifest is higher than the mod's own, in `patches/version`, so a newer build is never replaced by an older one. A dev build is offered the release of its own version too (see "Dev builds and releases"). Raise `patches/version` with each change to the mod that should reach Kobos. The manifest still has a `list` line for each patch list, for Kobo Patchers before 12, which read those. It installs with a restart. Because of the signature, plain HTTP is tried if TLS doesn't work.

To test without a cable, run `make mods` and `make serve LAN=1`, and put the address it prints in `.adds/patches/update-url` on the Kobo. Check then updates from your computer. Only builds signed with the key are taken, so this needs the key in `~/.config/kobo-patcher/signing-key.pem`.

Choices are saved in `/mnt/onboard/.adds/patches/choices.txt`, one `id on` or `id off` line for each patch switched on the Kobo, and made again when Nickel starts, before it has built anything. Patches that were never switched there stay as the files have them, so a Kobo patched the old way still works.

Most patches change code Nickel runs again whenever it's needed. For the rest, the kit's `nk.patch` (`kit/patch.cpp`) gives the nudge Nickel itself would:

- The bottom bar, the More page and the home screen are asked to update.
- A changed stylesheet is set again on the widgets showing the old one, found by its text, because Kobo's `setStyleSheetFile` doesn't keep the file's name. Widgets the old one hid with `qproperty-visible: false` are shown again.
- Some values are worked out once and kept, like the line spacing list. Clearing one of those needs addresses from one firmware's libnickel, listed in `known`, and checked against the running libnickel first. On a firmware that isn't listed, the Kobo asks for a restart instead.
- A patch with `rebuild: main-window` in `patches.yaml` has Kobo build its main window again, the bottom bar and then Home or My Books, with `nk.main_window.rebuild()`, as Kobo does after a USB connection. Any screen that was open closes, Kobo Patcher's too.

A patch should take effect straight away, with no restart. One whose code Nickel only runs once needs a nudge, a `known` entry, or a place where Kobo runs that code again by itself, such as `rebuild: main-window`. A restart is the last resort, for a change nothing Kobo does will show.

## KOReader

When `/mnt/onboard/.adds/koreader/koreader.sh` is there as Nickel starts, the Patches mod adds "KOReader" to the More page, above Kobo Patcher. Tapping it runs that launcher the way NickelMenu's entry for KOReader does: on its own, outside Nickel, with Nickel's environment. KOReader's launcher stops Nickel and starts it again when KOReader quits. So far this has only been tried in an emulator, with a stand-in launcher.
