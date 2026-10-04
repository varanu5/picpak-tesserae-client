#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
BUILD=$(mktemp -d "${TMPDIR:-/tmp}/picpak-battery-tests.XXXXXX")
trap 'rm -rf "$BUILD"' EXIT HUP INT TERM
# Use the production wake logic with host hardware mocks and public headers only.
for name in defaults lowbatt lowbatt_core manual_core button_event button_gesture connection_retry setup_check; do
    cp "firmware/main/$name.h" "$BUILD/"
done
for name in main lowbatt splash; do
    sed '/^#include /d; /^extern const uint8_t _binary_splash_/d' "firmware/main/$name.c" > "$BUILD/actual_$name.c"
done
cc -std=c11 -Wall -Wextra -Werror -I"$BUILD" \
    firmware/test/test_battery_wake.c -o "$BUILD/battery"
"$BUILD/battery"
