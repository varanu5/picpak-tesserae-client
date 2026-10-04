#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
BUILD=$(mktemp -d "${TMPDIR:-/tmp}/picpak-wake-tests.XXXXXX")
trap 'rm -rf "$BUILD"' EXIT HUP INT TERM

# Use public defaults without loading optional local credentials.
cp firmware/main/wake_align.c firmware/main/wake_align.h firmware/main/defaults.h \
    firmware/main/button_gesture.h "$BUILD/"
cc -std=c11 -Wall -Wextra -Werror \
    -Dgettimeofday=picpak_test_gettimeofday -Dsettimeofday=picpak_test_settimeofday \
    -Ifirmware/test/epd_stubs -I"$BUILD" \
    firmware/test/test_wake_align.c "$BUILD/wake_align.c" -lm -o "$BUILD/wake_align"
"$BUILD/wake_align"
