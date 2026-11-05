# Third-party notices

The community patches in `web/patches/` are compiled from
[kobopatch-patches](https://github.com/pgaskin/kobopatch-patches), by Patrick Gaskin, GeoffR and
its other contributors, under this licence:

```
MIT License

Copyright (c) 2018-2023 Patrick Gaskin

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## Kobo's firmware

The MIT licence in `LICENSE` doesn't cover Kobo's own code. The files in `web/patches/` hold small pieces of it, so the page can check what it's replacing and, for some patches, replace a compressed stylesheet whole. No firmware is stored here. You download it from Kobo.

## Add-ons

The page installs [KOReader](https://github.com/koreader/koreader) (AGPL-3.0) from a release file you download yourself. It isn't included here. The page can also remove [NickelMenu](https://github.com/pgaskin/NickelMenu) (MIT), which it used to install.

## The mod kit

The mods are built with [Lua](https://www.lua.org) 5.4.9 and [Monocypher](https://monocypher.org) 4.0.3, unchanged, which `mods/kit/vendor.sh` downloads from their releases for the build, under these licences:

```
Copyright (C) 1994-2026 Lua.org, PUC-Rio.

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

Monocypher's licence is in `mods/kit/licences/MONOCYPHER-LICENCE.md`.
