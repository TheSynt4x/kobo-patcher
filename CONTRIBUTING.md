# Contributing

Thanks for helping out. Patches here go onto other people's devices, so everything has to be tried on a real Kobo before it's merged.

You'll need Python with PyYAML (`pip install pyyaml`), `c++filt` (part of binutils, or Xcode on a Mac) and Node. The first run downloads the firmware into `~/.cache/kobo-patcher`, which is a few hundred MB.

## Adding a patch

Patches use the same format as [kobopatch](https://github.com/pgaskin/kobopatch-patches). If you've got one already, or want to bring one over from there, just copy it into a file:

```yaml
Hide the thing:
  - Enabled: no
  - Description: Hides the thing on the home screen.
  - BaseAddress: "SomeView::updateButtons()"
  - ReplaceBytes:
      Offset: 0xF6
      FindH: 01 21
      ReplaceH: 00 21
```

The title and description are what people see on the page, so keep them plain. Then run:

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

A patch that needs different bytes on each firmware goes in `patches.yaml` by hand, under `per_firmware`. The `no-discover` patch is an example.

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

Google Drive and Dropbox aren't compiled like the others. To offer them on your firmware, add `google_drive: true` or `dropbox: true` to the entry once you've tested it.

## Can't do either?

If your firmware's missing, [ask for it](https://github.com/TheSynt4x/kobo-patcher/issues/new?template=firmware.yml). Offering to test helps a lot.
