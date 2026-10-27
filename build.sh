#!/bin/sh
# build.sh - build + package Psychosis.
#
# On device: run as-is (picks up /var/jb tools and the on-device SDK).
# In CI:     SDK=$(xcrun --sdk iphoneos --show-sdk-path) bash build.sh
set -eu
umask 022
cd "$(dirname "$0")"

SDK="${SDK:-/var/jb/usr/share/SDKs/iPhoneOS.sdk}"
pick() { for c in "$@"; do command -v "$c" >/dev/null 2>&1 && { printf '%s' "$c"; return; }; done; }
CC="${CC:-$(pick /var/jb/usr/bin/cc clang)}"
LDID="${LDID:-$(pick /var/jb/usr/bin/ldid ldid)}"
DPKG="${DPKG:-$(pick /var/jb/usr/bin/dpkg-deb dpkg-deb)}"
for tool in "$CC" "$LDID" "$DPKG"; do
    [ -n "$tool" ] || { echo "missing tool (set CC/LDID/DPKG)" >&2; exit 1; }
done
[ -d "$SDK" ] || { echo "SDK not found: $SDK (set SDK)" >&2; exit 1; }

VERSION="$(sed -n '/^Version:/{s/^Version:[[:space:]]*//;p;q;}' layout/DEBIAN/control)"
[ -n "$VERSION" ] || { echo "layout/DEBIAN/control has no Version" >&2; exit 1; }
OUT="packages/com.alsinas.psychosis_${VERSION}_iphoneos-arm64.deb"
FLAGS="-arch arm64 -mios-version-min=15.0 -isysroot $SDK -fblocks"

SOURCES="src/psychosis_tweak.c src/axdump.c src/mdc_protocol.c src/mdc_overlay.c src/mdc_mainthread.c src/mdc_services.c src/mdc_keymap.c"
# shellcheck disable=SC2086
"$CC" $FLAGS -dynamiclib $SOURCES -o Psychosis.dylib
"$LDID" -S Psychosis.dylib
LIBDIR=layout/var/jb/Library/MobileSubstrate/DynamicLibraries
cp Psychosis.dylib "$LIBDIR/Psychosis.dylib"
# shellcheck disable=SC2086
"$CC" $FLAGS src/psychosis.c src/mdc_protocol.c -o psychosis
"$LDID" -S psychosis
mkdir -p layout/var/jb/usr/bin packages
cp psychosis layout/var/jb/usr/bin/psychosis
ln -sf psychosis layout/var/jb/usr/bin/psi
chmod 755 Psychosis.dylib psychosis layout/var/jb/usr/bin/psychosis
find layout -type d -exec chmod 755 {} \;
find layout/DEBIAN -type f -exec chmod 644 {} \;
find layout/var -type f ! -name '*.dylib' ! -name psychosis -exec chmod 644 {} \;
chmod 755 layout/var/jb/Library/MobileSubstrate/DynamicLibraries/Psychosis.dylib
"$DPKG" --root-owner-group -b layout "$OUT"
echo "BUILT: $OUT"
echo "install: sudo dpkg -i $OUT"
