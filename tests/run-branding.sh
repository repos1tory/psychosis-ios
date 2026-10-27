#!/bin/sh
# Run after build.sh. No valid command is dispatched to the device.
set -eu
cd "$(dirname "$0")/.."
test -x ./psychosis
test "$(readlink ./psi)" = psychosis
test "$(readlink layout/var/jb/usr/bin/psi)" = psychosis
test ! -e ./mdctl
test ! -L layout/var/jb/usr/bin/mdctl
test ! -e layout/var/jb/usr/bin/mdctl
grep -qx 'Package: com.alsinas.psychosis' control
grep -qx 'Package: com.alsinas.psychosis' layout/DEBIAN/control
grep -qx 'Name: Psychosis' layout/DEBIAN/control
for cli in ./psychosis ./psi; do
    status=0
    output=$("$cli" 2>&1) || status=$?
    test "$status" -eq 2
    case "$output" in
        "usage: $cli "*) ;;
        *) echo "FAIL: $cli usage branding" >&2; exit 1 ;;
    esac
    status=0
    output=$("$cli" tap not-a-number 0 2>&1) || status=$?
    test "$status" -eq 2
    case "$output" in
        'err invalid-number'*) ;;
        *) echo "FAIL: $cli parser rejection" >&2; exit 1 ;;
    esac
done
echo 'PASS: Psychosis/psi branding, aliases, retired command removal, parser rejection'
