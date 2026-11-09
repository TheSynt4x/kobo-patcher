#!/bin/sh
# Build a mod into build/lib<name>.so beside its folder: mods/build.sh patches. A mod folder
# anywhere else builds the same way, with mods/build.sh path/to/folder, using this kit where it is.
# Needs zig 0.17.0, and Python for gen_*.py and plugin names, with PyYAML for the patch list.
#
# A mod is a folder of .c and .cpp files, and vendor/*.c, or of Lua: a main.lua, other *.lua and
# icons/*.png, which are built into the .so, with Lua, Monocypher and the kit's Lua side. Its gen_*.py run
# first, from the folder above it, and write into build/. A plugin file names its Qt plugin class and key, 15 and 4
# characters, so Qt loads it beside the other mods; without one it gets the kit's. A C++ mod's icons/<name>.png
# are built in too, as icon_<name>[] and icon_<name>_size in <icons.inc>, for nk_more_row. A version file
# holds the mod's version, a whole number, which nk.updates offers newer ones by, and tools/sign_mod.mjs reads.
# A Lua mod gets the kit's own scripts too, such as kit/updates.lua for nk.updates.
#
# The kit and a mod's .cpp files are built against Qt 5.2.1's headers (kit/qt-headers.sh fetches
# them, and kit/vendor.sh fetches Lua and Monocypher), with no C++ library of their own: they use
# Nickel's Qt, and operator new and delete from Nickel's. RTTI is on, since Nickel may type-check
# any widget it delivers a gesture to.
#
# Every symbol the .so leaves undefined is listed with nm and built into it, with the names the kit
# looks up, so the kit can check them all before the mod starts. That takes two links.
set -e
export LC_ALL=C
mods="$(cd "$(dirname "$0")" && pwd)"
kit="$mods/kit"
folder="$1"
[ -d "$folder" ] || folder="$mods/$1"
[ -n "$1" ] && [ -d "$folder" ] || { echo "no mod at $1" >&2; exit 1; }
folder="$(cd "$folder" && pwd)"
name="$(basename "$folder")"
root="$(dirname "$folder")"
out="$root/build/$name"
so="$root/build/lib$name.so"
cd "$root"
for generator in "$folder"/gen_*.py; do [ -f "$generator" ] && python3 "$generator"; done
rm -rf "$out"
mkdir -p "$out"
if [ -f "$folder/plugin" ]; then
  python3 "$mods/rename_metadata.py" "$kit/metadata.inc" "$out/metadata.inc" $(cat "$folder/plugin")
else
  cp "$kit/metadata.inc" "$out/"
fi
qt="$("$kit/qt-headers.sh")"
vendor="$("$kit/vendor.sh")"

# What this build is, which the kit shows on Kobo's More page: "release" when the site's CI builds
# it with KOBO_RELEASE=1, otherwise "dev", the commit, + if the mod or the kit has changes not
# committed, and when. A release says nothing more, so the same code always builds the same bytes.
changed="$( { git -C "$folder" status --porcelain -- .; git -C "$kit" status --porcelain -- .; } 2>/dev/null)"
if [ "$KOBO_RELEASE" = 1 ]; then
  [ -z "$changed" ] || { echo "a release is built from committed code, and $name or the kit has changes" >&2; exit 1; }
  stamp=release
else
  stamp="dev $(git -C "$folder" rev-parse --short HEAD 2>/dev/null || echo unknown)${changed:++} $(date '+%Y-%m-%d %H:%M')"
fi
version=
if [ -f "$folder/version" ]; then
  version="$(tr -d ' \r\n' < "$folder/version")"
  case "$version" in
  '' | *[!0-9]* | 0*) echo "$folder/version should be a whole number from 1, not '$version'" >&2; exit 1 ;;
  esac
fi

flags="-target arm-linux-gnueabihf.2.17 -mcpu=cortex_a7 -Os -fvisibility=hidden -ffunction-sections -fdata-sections
  -fno-sanitize=all -fno-unwind-tables -fno-asynchronous-unwind-tables -g0 -fPIC"
cxx() {
  zig c++ -c $flags -std=c++11 -fno-exceptions -frtti -fvisibility-inlines-hidden -Wno-gnu-inline-cpp-without-extern \
    -DNK_NAME="\"$name\"" -DNK_BUILD="\"$stamp\"" ${version:+-DNK_VERSION=$version} -I"$out" -I"$kit" -I"$vendor" -I"$kit/qt" -I"$qt" -I"$qt/QtCore" -I"$qt/QtGui" -I"$qt/QtWidgets" -I"$qt/QtNetwork" -I"$qt/QtSql" \
    -o "$2" "$1"
}
objects=
libs=
if [ ! -f "$folder/main.lua" ]; then
  : > "$out/icons.inc"
  for file in "$folder"/icons/*.png; do
    [ -f "$file" ] || continue
    icon="icon_$(basename "$file" .png | tr -c 'A-Za-z0-9\n' _)"
    printf 'static const unsigned char %s[] = {' "$icon" >> "$out/icons.inc"
    od -An -v -tu1 "$file" | tr -s ' \n' ',' | sed 's/^,//; s/,$//' >> "$out/icons.inc"
    printf '};\nstatic const unsigned %s_size = %s;\n' "$icon" "$(wc -c < "$file")" >> "$out/icons.inc"
  done
fi
for source in "$folder"/*.c "$folder"/vendor/*.c; do
  [ -f "$source" ] || continue
  object="$out/$(basename "$source" .c).o"
  zig cc -c $flags -w -DNK_NAME="\"$name\"" -I"$root/build" -I"$kit" -I"$vendor" -o "$object" "$source"
  objects="$objects $object"
done
for source in "$folder"/*.cpp; do
  [ -f "$source" ] || continue
  object="$out/$(basename "$source" .cpp).o"
  cxx "$source" "$object"
  objects="$objects $object"
done
if [ -f "$folder/main.lua" ]; then
  embedded="$out/embedded.inc"
  : > "$embedded"
  table=
  i=0
  for file in "$folder"/*.lua "$folder"/icons/*.png "$kit"/*.lua; do
    [ -f "$file" ] || continue
    case "$file" in
    "$kit"/*) path="kit/${file#"$kit"/}" ;;
    *) path="${file#"$folder"/}" ;;
    esac
    printf 'static const unsigned char embedded_%d[] = {' "$i" >> "$embedded"
    od -An -v -tu1 "$file" | tr -s ' \n' ',' | sed 's/^,//' >> "$embedded"
    printf '0};\n' >> "$embedded"
    table="$table  {\"$path\", embedded_$i, $(wc -c < "$file")},\n"
    i=$((i + 1))
  done
  printf 'static const Embedded embedded[] = {\n%b};\n' "$table" >> "$embedded"
  for source in "$vendor"/vendor/lua/*.c "$vendor"/vendor/monocypher/*.c; do
    object="$out/vendor_$(basename "$source" .c).o"
    zig cc -c $flags -w -o "$object" "$source"
    objects="$objects $object"
  done
  cxx "$kit/lua.cpp" "$out/lua.o"
  cxx "$kit/tools.cpp" "$out/tools.o"
  cxx "$kit/patch.cpp" "$out/patch.o"
  cxx "$kit/ui.cpp" "$out/ui.o"
  cxx "$kit/books.cpp" "$out/books.o"
  cxx "$kit/dictionaries.cpp" "$out/dictionaries.o"
  objects="$objects $out/lua.o $out/tools.o $out/patch.o $out/ui.o $out/books.o $out/dictionaries.o"
  libs=-lm
fi
link() {
  cxx "$kit/nk.cpp" "$out/nk.o"
  zig cc $flags -w -shared -Wl,--gc-sections -Wl,-z,norelro -Wl,-z,lazy \
    -Wl,--hash-style=sysv -o "$so" "$out/nk.o" $objects -ldl -lpthread $libs
}
undefined() { nm -D --undefined-only "$so" | awk '$1 == "U" { sub(/@.*/, "", $2); print $2 }' | sort -u; }
: > "$out/symbols.inc"
link
undefined > "$out/undefined.txt"
{ cat "$out/undefined.txt"; grep -o '"_Z[^"]*"' "$kit/nk.cpp" | tr -d '"'; } | sort -u | sed 's/.*/"&",/' > "$out/symbols.inc"
link
undefined | cmp -s - "$out/undefined.txt" || { echo "the second link needs other symbols" >&2; exit 1; }
echo "$so"
