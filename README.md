# Kobo Patcher

Patch a Kobo from your browser: connect it, tick what you want, and patch. There's nothing to install, and every change can be undone.

**[kobo.thesynt4x.dev](https://kobo.thesynt4x.dev)** needs Chrome or Edge, because other browsers can't open a folder on your drive.

Supported: Kobo Nia on 4.38.23852 and Kobo Libra Colour on 4.46.23852. Other models and firmware are refused.

Patching system files can leave a Kobo needing a factory reset, so use this at your own risk and run **Back up everything** first. A firmware update removes the patches. This project isn't affiliated with Kobo.

## How it works

The page runs entirely in your browser and uploads nothing. It reads the system files from a firmware zip you download from Kobo, checks their hashes, applies exact byte edits, and writes a single `.kobo/KoboRoot.tgz` that the Kobo installs on restart. Each edit is checked against the bytes it expects, so a mismatch stops the patch. Changing your patches rebuilds from the originals, so anything you untick goes away.

## Adding a patch or firmware

See [CONTRIBUTING.md](CONTRIBUTING.md). Patches use kobopatch's format, and `make add FILE=my-patch.yaml` adds one.

CI applies every patch to the real firmware, and merging to `main` deploys the page once tests pass.

## Credits

Community patches come from [kobopatch-patches](https://github.com/pgaskin/kobopatch-patches) by Patrick Gaskin, GeoffR and its other contributors. Thanks also to [NickelMenu](https://github.com/pgaskin/NickelMenu) and [KOReader](https://github.com/koreader/koreader). See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Licence

[MIT](LICENSE), apart from the pieces of Kobo's code described in the notices.
