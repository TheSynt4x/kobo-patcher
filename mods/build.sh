#!/bin/sh
# Build a mod into build/lib<name>.so: mods/build.sh patches
# Needs zig (for its ARM cross-compiler) and, for the patch list, Python with PyYAML.
set -e
cd "$(dirname "$0")"
name="$1"
for generator in "$name"/gen_*.py; do [ -f "$generator" ] && python3 "$generator"; done
mkdir -p build
grep -o '"_Z[^"]*"' kit/nk.c | sort -u | sed 's/$/,/' > build/symbols.inc
zig cc -target arm-linux-gnueabihf.2.17 -mcpu=cortex_a7 -Os -fno-sanitize=all -fno-unwind-tables -fno-asynchronous-unwind-tables -g0 -shared -fPIC \
  -Wl,-z,norelro -Wl,-z,lazy -Wl,--hash-style=sysv -w -DNK_NAME="\"$name\"" \
  -Ibuild -o "build/lib$name.so" kit/nk.c "$name"/*.c -ldl -lpthread
