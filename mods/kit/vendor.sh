#!/bin/sh
# Lua 5.4.9 and Monocypher 4.0.3, unchanged, for the kit: Lua runs the mods' scripts, and Monocypher
# checks Ed25519 signatures for nk.crypto.ed25519_verify, which Kobo Patcher uses on its updates.
# They go in mods/build/kit-vendor/vendor/{lua,monocypher}, from each project's release, checked
# against its SHA-256, so the kit's #include "vendor/lua/lua.h" finds them there. Prints the folder
# to add to the include path.
#
# Only Lua's core and its base, coroutine, table, string, math and utf8 libraries are kept: mods reach
# files, the network and the rest through the kit. Their licences are in THIRD_PARTY_NOTICES.md and
# kit/licences.
set -e
cd "$(dirname "$0")/.."
lua=5.4.9
lua_sum=2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6
mono=4.0.3
mono_sum=8cc9bc341a66249016db9bd70e9142d8d0aef9945973744b1ac05dbc55d8ee66
dir=build/kit-vendor
out="$dir/vendor"
stamp="$out/.lua-$lua-monocypher-$mono"
if [ -f "$stamp" ]; then
  echo "$PWD/$dir"
  exit 0
fi
mkdir -p "$dir"

# Downloads the first of these that has the file, and checks it.
fetch() {
  file="$1"; sum="$2"; shift 2
  if [ -f "$dir/$file" ] && echo "$sum  $dir/$file" | sha256sum -c --quiet 2>/dev/null; then return; fi
  for url in "$@"; do
    curl -sSfL --max-time 120 -o "$dir/$file.part" "$url/$file" && mv "$dir/$file.part" "$dir/$file" && break
    echo "couldn't download $url/$file, trying the next" >&2
  done
  rm -f "$dir/$file.part"
  [ -f "$dir/$file" ] || { echo "couldn't download $file" >&2; exit 1; }
  echo "$sum  $dir/$file" | sha256sum -c --quiet || { rm -f "$dir/$file"; exit 1; }
}
fetch "lua-$lua.tar.gz" "$lua_sum" https://www.lua.org/ftp https://www.tecgraf.puc-rio.br/lua/ftp
fetch "monocypher-$mono.tar.gz" "$mono_sum" https://monocypher.org/download

rm -rf "$out" "$dir/lua-$lua" "$dir/monocypher-$mono"
mkdir -p "$out/lua" "$out/monocypher"
tar -xzf "$dir/lua-$lua.tar.gz" -C "$dir"
tar -xzf "$dir/monocypher-$mono.tar.gz" -C "$dir"
for file in "$dir/lua-$lua/src/"*.c "$dir/lua-$lua/src/"*.h; do
  case "$(basename "$file")" in
    lua.c|luac.c|linit.c|loadlib.c|liolib.c|loslib.c|ldblib.c) ;;
    *) cp "$file" "$out/lua/" ;;
  esac
done
cp "$dir/monocypher-$mono/src/monocypher.c" "$dir/monocypher-$mono/src/monocypher.h" \
  "$dir/monocypher-$mono/src/optional/monocypher-ed25519.c" "$dir/monocypher-$mono/src/optional/monocypher-ed25519.h" \
  "$out/monocypher/"
rm -rf "$dir/lua-$lua" "$dir/monocypher-$mono"
touch "$stamp"
echo "$PWD/$dir"
