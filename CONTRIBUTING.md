# Contributing

Thanks for helping out. Patches here go onto other people's devices, so everything has to be tried on a real Kobo before it's merged.

You'll need Python with PyYAML (`pip install pyyaml`), `c++filt` (part of binutils, or Xcode on a Mac) and Node. The first run downloads the firmware into `~/.cache/kobo-patcher`, which is a few hundred MB.

## Adding a patch

Patches use the same format as [kobopatch](https://github.com/pgaskin/kobopatch-patches). If you've got one already, or want to bring one over from there, just copy it into a file:

```yaml
Hide the thing:
  - Enabled: no
  - Description: Hides the thing on the home screen.
  - Category: Ads and upsells
  - BaseAddress: "SomeView::updateButtons()"
  - ReplaceBytes:
      Offset: 0xF6
      FindH: 01 21
      ReplaceH: 00 21
```

The title and description are what people see on the page, so keep them plain. `Category` isn't part of kobopatch's format, so add it yourself. It's the section the patch appears under, and has to be one of the `categories` in `patches.yaml`. Then run:

```bash
make add FILE=my-patch.yaml
```

It tells you which firmware the patch fits, adds it to `patches.yaml` and updates `web/patches/`. The page only offers a patch on firmware where it fits, so it's fine if it doesn't fit all of them.

Next, try it:

```bash
make test
make serve
```

Open http://localhost:8765 in Chrome or Edge, patch your Kobo, and check it did what you expected. Then open a pull request, and commit the changes to `patches.yaml` and `web/patches/`.

The page only offers the Patches mod once `make mods` has built it into `web/mods/`, which needs zig. The site builds it when it deploys, so don't commit it. The mod reads its patch list from a file, which `make mods` writes to `web/mods/lists/`. To try a patch with the mod, copy your Kobo's list from `web/mods/lists/` to `.adds/patches/lists/` on it and open Kobo Patcher again: there's no need to reinstall anything. If you have the signing key, it can come over Wi-Fi instead: see `mods/README.md`. The mod switches patches straight away. `make patches` notices a patch to code Nickel runs only once, and the mod then asks for a restart. If yours only shows after a restart anyway, add `restart: yes` to it, and see "The Patches mod" in `mods/README.md`.

A patch that needs different bytes on each firmware goes in `patches.yaml` by hand, under `per_firmware`. The `no-more-extras` patch is an example. In `ReplaceH`, `BL{Some::function()}` stands for a Thumb `bl` from that place to a function in the same file, so code that calls Kobo's functions doesn't need its branches worked out by hand.

Often a patch only has to change what one call returns, and then `ReplaceCall` saves writing it for each firmware. After a `BaseAddress`, `ReplaceCall: {Call: "Some::check()", ReplaceH: "0120 00BF"}` finds the one call to `Some::check()` in that function and replaces its 4 bytes, here with `movs r0, #1; nop`, so the function goes on as if the check said yes. `ReplaceInstNOP: true` skips the call instead. It needs no offset, and refuses a function with no such call or more than one. `no-kobo-store` is made of them.

A category listed under `all_switch` in `patches.yaml` gets one more switch, "All" and the category's name, on the page and in Kobo Patcher, which turns every patch in it on or off.

## Adding a firmware

Add it under `firmware` in `patches.yaml`. Adding a version never removes the others:

```yaml
- model: N306
  name: Kobo Nia
  firmware: "4.39.99999"
  url: https://ereaderfiles.kobo.com/firmwares/kobo7/.../kobo-update-4.39.99999.zip
```

`model` is the first 4 characters of the serial in `.kobo/version`. Download links are in the [firmware list](https://pgaskin.net/KoboStuff/kobofirmware.html).

Then run `make patches`. It lists which patches don't fit, and those just won't be offered on that firmware, so you can open a pull request without fixing them all. Test what's left on a real Kobo first.

Google Drive and Dropbox are compiled like the others, but they're only offered on firmware marked `google_drive: true` or `dropbox: true`, through their `only_on`, because they also need Kobo's address for linking, and that has to be tried. Add the flag to the entry once you've tested them. A patch's `settings` are written to `Kobo eReader.conf`, its values when it goes on and its `when_off` values when it goes off: by the page as it's ticked or unticked, and by Kobo Patcher, straight away, as it's switched there.

The Patches mod gets a list for every firmware here, so Kobos that already have it pick up the new patches when they check for updates. The page only offers the mod, and the mod only updates itself, on firmware with `mod: true`. Add it once you've installed the mod on a real Kobo with that firmware, and switched a few patches on and off in Kobo Patcher.

## Can't do either?

If your firmware's missing, [ask for it](https://github.com/TheSynt4x/kobo-patcher/issues/new?template=firmware.yml). Offering to test helps a lot.
