.PHONY: patches mods add test serve

# Compile patches.yaml into web/patches, downloading what it needs the first time.
patches:
	python3 tools/compile_patches.py

# Build the mods the page offers into web/mods, with the patch list for each firmware in
# web/mods/lists. Needs zig, and Python with PyYAML.
mods:
	sh mods/build.sh patches
	mkdir -p web/mods
	cp mods/build/libpatches.so web/mods/
	python3 tools/write_lists.py
	node tools/sign_mods.mjs

# Add a patch written in kobopatch's format: make add FILE=my-patch.yaml
add:
	python3 tools/add_patch.py $(FILE)

test:
	node --test tests/patcher.test.mjs
	python3 tests/test_add_patch.py

# Serve the page at http://localhost:8765. With LAN=1, to your Wi-Fi too, for a Kobo to update from.
serve:
	python3 tools/serve.py $(if $(LAN),--lan)
