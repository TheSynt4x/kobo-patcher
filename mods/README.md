# Mods

Mods add to Kobo's UI using Kobo's own widgets, so a new row or screen looks like it was always there.
A mod is a small C or C++ file on top of `kit/`. The kit is C++, built against Qt 5.2.1's headers, the Qt that Nickel runs, and uses Nickel's own Qt when it runs. There's no Qt to install: the build fetches the headers.

```c
#include "../kit/nk.h"

static void build(NkScreen *screen, void *arg) {
    nk_section(screen, "HELLO");
    nk_value_row(screen, "Greeting", "Hi");
    nk_toggle_row(screen, "Be friendly", 1, on_change, 0);
    nk_button_row(screen, "Say it", "Start", on_start, 0);
}

static void open_screen(void *arg) { nk_open_screen("Hello", "More", build, 0); }

void nk_mod_init(void) { nk_more_row("Hello", icon, sizeof icon, open_screen, 0); }
```

Build it with `mods/build.sh <folder>`, which needs zig 0.17.0, curl the first time (for Qt's headers, which `kit/qt-headers.sh` fetches into `build/qt` and checks), and Python with PyYAML for `patches`. It writes
`build/lib<folder>.so`, which goes in `/usr/local/Kobo/imageformats/` on the Kobo. A folder elsewhere builds the same way, with `build.sh path/to/folder`, and its `.so` goes in `build/` beside that folder. A `plugin` file in the folder gives it its own Qt plugin class and key, 15 and 4 characters, so Qt loads it beside other mods.

**Status: works on a real Kobo Nia on firmware 4.38.23852 and a real Kobo Libra Colour on 4.46.23852, one device each. Not yet tried on other models. The crash protection below has been tested on the Nia for a crash while a mod starts, not yet for one while a screen opens. The missing-symbol check has only been tested in an emulator. Switching patches straight away has only been tested in an emulator so far.**

## What keeps a Kobo safe

- Every symbol the mod uses is checked when Nickel loads it, before the mod does anything. The build lists them: what the `.so` leaves undefined, and the names the kit looks up. If one is missing, the mod does nothing and there's no crash.
- Before a screen opens, the controller's vtable is compared with what's expected.
- If Nickel crashes while a mod starts or opens a screen, or soon after `nk_failsafe`, the mod is moved to `/usr/local/Kobo/<name>.disabled` and isn't loaded again. The Kobo may freeze or restart first. If it's stuck, hold power for about 15 seconds and turn it on again. Nickel exiting normally, `nk_reboot`, or a command from `nk_spawn` stopping Nickel, doesn't count as a crash.
- `/mnt/onboard/.adds/<name>/uninstall` removes the mod the next time Nickel starts.

A crash after a screen has opened isn't caught.

## The Patches mod

It adds "Kobo Patcher" at the top of Settings, like other apps' patchers do, and switches patches on and off in Nickel's memory, straight away, and never changes the firmware's files. Each patch's edits are found in libnickel or the nickel program as Nickel has loaded them, and every edit's bytes are checked before any are written. Bytes that match neither side of a patch show as "Changed" and are left alone. A patch with some edits on and some off, as an older version of it leaves the files, shows as off, and switching it finishes or undoes it.

The patch list is a file, `/mnt/onboard/.adds/patches/lists/<model>-<firmware>.txt`, written by `tools/write_lists.py`. It's read when Nickel starts and again whenever Kobo Patcher opens and the file has changed, so new patches arrive without reinstalling the mod. Every line is checked, and edits may only touch libnickel and the nickel program.

"New patches and Kobo Patcher" checks for updates over Wi-Fi, through Qt's own network code. It downloads `mods/manifest.txt` from the site, which `tools/sign_mods.mjs` signs with an Ed25519 key, and refuses it unless the signature matches `UPDATE_KEY` in `update.c` ([Monocypher](patches/vendor/README.md) checks it). Every other file has to match the SHA-512 and size the manifest gives it. A new patch list is saved straight away. A new mod is only offered on the firmware the manifest names, the ones marked `mod: true` in `patches.yaml`, and installs with a restart. Because of the signature, a TLS error is ignored, and plain HTTP is tried if TLS can't be used at all.

To test without a cable, run `make mods` and `make serve LAN=1`, and put the address it prints in `.adds/patches/update-url` on the Kobo. Check then updates from your computer. Only builds signed with the key are taken, so this needs the key in `~/.config/kobo-patcher/signing-key.pem`.

Choices are saved in `/mnt/onboard/.adds/patches/choices.txt`, one `id on` or `id off` line for each patch switched on the Kobo, and made again when Nickel starts, before it has built anything. Patches that were never switched there stay as the files have them, so a Kobo patched the old way still works.

Most patches change code Nickel runs again whenever it's needed. For the rest, `live.c` gives the nudge Nickel itself would:

- The bottom bar, the More page and the home screen are asked to update.
- A changed stylesheet is set again on the widgets showing the old one, found by its text, because Kobo's `setStyleSheetFile` doesn't keep the file's name. Widgets the old one hid with `qproperty-visible: false` are shown again.
- Some values are worked out once and kept, like the line spacing list. Clearing one of those needs addresses from one firmware's libnickel, listed in `known`, and checked against the running libnickel first. On a firmware that isn't listed, the Kobo asks for a restart instead.

A new patch whose code Nickel only runs once may need a nudge or a `known` entry, or it will only take effect after a restart.

## KOReader

When `/mnt/onboard/.adds/koreader/koreader.sh` is there as Nickel starts, the Patches mod adds "KOReader" to the More page, above Settings. Tapping it runs that launcher the way NickelMenu's entry for KOReader does: on its own, outside Nickel, with Nickel's environment. KOReader's launcher stops Nickel and starts it again when KOReader quits. So far this has only been tried in an emulator, with a stand-in launcher.
