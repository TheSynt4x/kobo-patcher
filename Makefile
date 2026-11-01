.PHONY: patches test serve

# Compile patches.yaml into web/patches, downloading what it needs the first time.
patches:
	python3 tools/compile_patches.py

test:
	node --test tests/patcher.test.mjs

# Serve the page at http://localhost:8765
serve:
	python3 tools/serve.py
