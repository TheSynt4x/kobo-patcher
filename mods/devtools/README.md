# devtools

Development tools on a real Kobo, driven from the computer over Wi-Fi with `mods/tools/kobo`. On the device itself, it does what a developer needs from it: taps and swipes as real Qt touch events, the visible widgets with their text and places, screenshots, every mod's log, files, shell commands, and reloading a Lua mod's scripts.

It's a plain C++ mod on `nk.h`, without the Lua kit, so a broken kit can't take the tools down with it. It listens on port 8484 only if `.adds/devtools/token` exists, and answers only requests that carry that token. Anyone on the network with the token is root on the Kobo, so keep it to your own Wi-Fi, and remove the token, or the mod, when you're not developing.

## Setting up

Once, over USB:

```bash
mods/tools/kobo setup          # builds devtools, puts it in .kobo/KoboRoot.tgz with a new token
```

Eject the Kobo, let it install and restart, and connect it to Wi-Fi. Then:

```bash
mods/tools/kobo find           # finds it on this network and remembers it in build/device
```

Kobo turns Wi-Fi off when it's idle. To keep it on while developing, add this to `.kobo/Kobo/Kobo eReader.conf` over USB:

```
[DeveloperSettings]
ForceWifiOn=true
```

## Using it

`mods/tools/kobo help` lists the commands. The ones used most:

```bash
mods/tools/kobo push patches         # scripts to .adds/patches/dev, and main.lua run again
mods/tools/kobo tap Settings         # tap the widget that says Settings, and wait until it settles
mods/tools/kobo tree                 # what's on screen, with text and places
mods/tools/kobo shot                 # build/shot.png
mods/tools/kobo log                  # what mods have written since the last time
mods/tools/kobo eval patches 'return nk.device.firmware()'
mods/tools/kobo install patches      # a C++ or kit change: build, install, reboot, wait
mods/tools/kobo undev patches        # back to the built-in scripts
mods/tools/kobo status               # each mod: the release at its own address, or a DEV BUILD and which; probation; dev switches left on
mods/tools/kobo reset                # back to what the site gives: no update-url or pushed scripts, the site's Kobo Patcher
```

`reset` leaves devtools, which it goes through, and mods the site has no release of.

## Switching it off

"Developer tools" on Kobo's More page has a switch. Off, it stops listening straight away and leaves `.adds/devtools/off`, so it stays off after a restart. On again, it first shows Kobo's dialog with a warning: anyone on the Wi-Fi who has the token can control the Kobo, its files and its shell. A Kobo set up with `mods/tools/kobo setup` starts with it on. Tried in an emulator on 5 November: off, on with the warning, and off across a restart.

## When Nickel crashes

A crash in Nickel restarts the Kobo, and the log goes with it. So devtools catches the fatal signal first and appends a report to `.adds/devtools/crash.log`: the signal and address, the library and offset of the program counter and return address, the code addresses on the stack, and the end of the log. `mods/tools/kobo crash` prints it. `mods/tools/kobo log -f` keeps following across restarts, so the log up to a crash is on the computer too.

An offset in the report is from the start of the library's mapping. For a mod's `.so`, add the start of the segment it's in, as `llvm-readelf -lW` shows it (rounded down to 0x1000), and disassemble there with `nix shell nixpkgs#llvm -c llvm-objdump -d --triple=armv7-linux-gnueabihf`.

A tap by text picks the last widget that says it, which is the one drawn on top, and lists the others if there are several (`--nth N` picks one). Text matches exactly first, then as whole words, so `tap OK` never hits "BOOKS". `tree` shows tick boxes as `[x]` and `[ ]`. Taps, presses and swipes return once nothing has been painted for 400 ms, rather than after a fixed wait.

`push` only works for Lua mods built with a kit that has `nk_dev_reload` and `nk_dev_eval` (`kit/lua.cpp`). Installing such a build once with `install` is enough; after that, scripts go over with `push`.

If you have an emulator of your own running Kobo's firmware, `--emu` before any command drives it on this computer that forwards 127.0.0.1:18484 to its port 8484, with the token in `build/emulator`. Mods in other folders are found through `KOBO_MODS`, a list of folders separated by colons, and `KOBO_BUILD` moves where the Kobo and its token are kept.

## Kobo's Qt

Kobo's Qt isn't stock 5.2.1: its `QIODevice` has two more virtual functions, so every virtual function of a socket, file or process after them is two vtable slots further on than Qt's headers say. Calling one through the vtable calls the wrong function. Here, calling `disconnectFromHost()` that way crashed Nickel in an emulator. So devtools calls them by name (`socket->QAbstractSocket::disconnectFromHost()`).

## Tested

On 4 November 2026, in an emulator running the Nia's firmware (4.38.23852): ping, tree, tap by text, settling, screenshot, push and reload, eval (including an error and code that waits), log, shell. Then on the real Nia (4.38.23852): setup over USB, find, tree, tap by text, screenshot, log, install with a reboot (57 s from build to back), push and reload (under a second). Then the crash report, which caught devtools' own bug: a shell command's timeout used its process after it was deleted, so Nickel crashed about 20 s after some `tools/kobo sh` calls and the Kobo restarted. Fixed, and ten shell calls with uploads and taps after it ran without a crash. Not tried on the Libra Colour.
