#!/bin/sh
# Host-side parser tests for psychosis. Pure C, no iOS frameworks needed,
# so this runs on the device (or any Mac/Linux host with cc).
set -e
cd "$(dirname "$0")/.."
BIN="tests/.mdc_protocol_tests"
trap 'rm -f "$BIN"' EXIT
cc -Wall -Wextra -O1 src/mdc_protocol.c tests/test_protocol.c -o "$BIN"
"$BIN"
