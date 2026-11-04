#!/bin/sh
# Qt 5.2.1's headers, the version Nickel runs, for the kit and the mods written in C++. They go in
# mods/build/qt/qtbase-opensource-src-5.2.1/include, from qtbase's source release, checked against
# its SHA-256. kit/qt holds the two headers Qt's configure would have written.
#
# One fix for today's clang: Qt 5.2 tells signed from unsigned with T(0) < T(-1), which clang no
# longer accepts for an enum. The enum's underlying type gives the same answer, as GCC gave when
# Kobo's Qt was built, so QFlags keeps its layout.
set -e
cd "$(dirname "$0")/.."
version=5.2.1
sum=acdfd1aa2548ebea1d922e8e24e5c59f5fc3b2beae7c8003ba47d773bfcc94c0
dir=build/qt
src="$dir/qtbase-opensource-src-$version"
if [ -f "$src/include/QtCore/qglobal.h" ] && grep -q underlying_type "$src/src/corelib/global/qtypetraits.h"; then
  echo "$PWD/$src/include"
  exit 0
fi
mkdir -p "$dir"
tarball="$dir/qtbase-opensource-src-$version.tar.xz"
[ -f "$tarball" ] || curl -sSfL -o "$tarball" "https://download.qt.io/archive/qt/5.2/$version/submodules/qtbase-opensource-src-$version.tar.xz"
echo "$sum  $tarball" | sha256sum -c --quiet
rm -rf "$src"
tar -xJf "$tarball" -C "$dir" "qtbase-opensource-src-$version/include" "qtbase-opensource-src-$version/src"
traits="$src/src/corelib/global/qtypetraits.h"
sed -i 's|^    : integral_constant<bool, (T(0) < T(-1))> {};|    : integral_constant<bool, std::is_unsigned<typename std::conditional<std::is_enum<T>::value, std::underlying_type<T>, std::common_type<T> >::type::type>::value> {};|' "$traits"
sed -i 's|^#include <utility>                  // For pair|&\n#include <type_traits>|' "$traits"
grep -q 'std::underlying_type<T>' "$traits" || { echo "couldn't fix $traits" >&2; exit 1; }
echo "$PWD/$src/include"
