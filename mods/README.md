# Mods

Mods add to Kobo's UI using Kobo's own widgets, so a new row or screen looks like it was always there.
A mod is a small C file on top of `kit/`. There's no Qt to install: symbols are looked up at runtime.

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

Build it with `mods/build.sh <folder>`, which needs zig, and Python with PyYAML for `patches`. It writes
`build/lib<folder>.so`, which goes in `/usr/local/Kobo/imageformats/` on the Kobo.

**Status: works on a real Kobo Nia on firmware 4.38.23852, tested on one device. Not yet tried on other models. The crash protection below has been tested on the Nia for a crash while a mod starts, not yet for one while a screen opens. The missing-symbol check has only been tested in an emulator. Switching patches straight away has only been tested in an emulator so far.**

## What keeps a Kobo safe

- Every symbol is checked before anything changes. If one is missing there's no row and no crash.
- Before a screen opens, the controller's vtable is compared with what's expected.
- If Nickel crashes while a mod starts or opens a screen, or soon after `nk_failsafe`, the mod is moved to `/usr/local/Kobo/<name>.disabled` and isn't loaded again. The Kobo may freeze or restart first. If it's stuck, hold power for about 15 seconds and turn it on again. Nickel exiting normally, or `nk_reboot`, doesn't count as a crash.
- `/mnt/onboard/.adds/<name>/uninstall` removes the mod the next time Nickel starts.

A crash after a screen has opened isn't caught.

## The Patches mod

It adds "Kobo Patcher" at the top of Settings, like other apps' patchers do, and switches patches on and off in Nickel's memory, straight away, and never changes the firmware's files. Each patch's edits are found in libnickel or the nickel program as Nickel has loaded them, and every edit's bytes are checked before any are written. Bytes that match neither side of a patch show as "Changed" and are left alone.

Choices are saved in `/mnt/onboard/.adds/patches/choices.txt`, one `id on` or `id off` line for each patch switched on the Kobo, and made again when Nickel starts, before it has built anything. Patches that were never switched there stay as the files have them, so a Kobo patched the old way still works.

Most patches change code Nickel runs again whenever it's needed. For the rest, `live.c` gives the nudge Nickel itself would:

- The bottom bar, the More page and the home screen are asked to update.
- A changed stylesheet is set again on the widgets showing the old one, found by its text, because Kobo's `setStyleSheetFile` doesn't keep the file's name. Widgets the old one hid with `qproperty-visible: false` are shown again.
- Some values are worked out once and kept, like the line spacing list. Clearing one of those needs addresses from one firmware's libnickel, listed in `known`, and checked against the running libnickel first. On a firmware that isn't listed, the Kobo asks for a restart instead.

A new patch whose code Nickel only runs once may need a nudge or a `known` entry, or it will only take effect after a restart.
