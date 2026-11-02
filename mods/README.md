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

**Status: works on a real Kobo Nia on firmware 4.38.23852, tested on one device. Not yet tried on other models. The crash protection below has been tested on the Nia for a crash while a mod starts, not yet for one while a screen opens. The missing-symbol check has only been tested in an emulator.**

## What keeps a Kobo safe

- Every symbol is checked before anything changes. If one is missing there's no row and no crash.
- Before a screen opens, the controller's vtable is compared with what's expected.
- If Nickel crashes while a mod starts or opens a screen, the mod is moved to `/usr/local/Kobo/<name>.disabled` and isn't loaded again. The Kobo may freeze or restart first. If it's stuck, hold power for about 15 seconds and turn it on again.
- `/mnt/onboard/.adds/<name>/uninstall` removes the mod the next time Nickel starts.

A crash after a screen has opened isn't caught.

## The Patches mod

It reads each patch's state from the firmware's files. Apply writes `.kobo/KoboRoot.tgz` with the changed files, built from what's on the Kobo now, then restarts so that Kobo's startup script installs it. Bytes that match neither side of a patch show as "Changed" and are left alone. It won't start if a package is already waiting or there isn't room.
