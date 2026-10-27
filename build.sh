#!/bin/sh
# build.sh - build + package Psychosis on-device. Run from psychosis/.
set -e
export PATH=/var/jb/usr/bin:$PATH
SOURCES="src/psychosis_tweak.c src/axdump.c src/mdc_protocol.c src/mdc_overlay.c src/mdc_mainthread.c src/mdc_services.c src/mdc_keymap.c"
cc -fblocks -dynamiclib $SOURCES -o Psychosis.dylib
ldid -S Psychosis.dylib
LIBDIR=layout/var/jb/Library/MobileSubstrate/DynamicLibraries
cp Psychosis.dylib "$LIBDIR/Psychosis.dylib"
cc src/psychosis.c src/mdc_protocol.c -o psychosis
ldid -S psychosis
mkdir -p layout/var/jb/usr/bin
cp psychosis layout/var/jb/usr/bin/psychosis
ln -sf psychosis layout/var/jb/usr/bin/psi
ln -sf psychosis psi
chmod 755 Psychosis.dylib psychosis layout/var/jb/usr/bin/psychosis
find layout -type d -exec chmod 755 {} \;
find layout/DEBIAN -type f -exec chmod 644 {} \;
find layout/var -type f ! -name '*.dylib' ! -name psychosis -exec chmod 644 {} \;
chmod 755 layout/var/jb/Library/MobileSubstrate/DynamicLibraries/Psychosis.dylib
dpkg-deb -b layout com.alsinas.psychosis_0.7.1_iphoneos-arm64.deb
echo BUILT: com.alsinas.psychosis_0.7.1_iphoneos-arm64.deb
echo "install: sudo dpkg -i com.alsinas.psychosis_0.7.1_iphoneos-arm64.deb"
echo "Reload SpringBoard separately only when authorized."
