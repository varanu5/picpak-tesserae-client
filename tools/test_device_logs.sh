#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
BUILD=$(mktemp -d "${TMPDIR:-/tmp}/picpak-log-tests.XXXXXX")
trap 'rm -rf "$BUILD"' EXIT HUP INT TERM
for name in log_capture log_ring diag; do
    cp "firmware/main/$name.c" "firmware/main/$name.h" "$BUILD/"
done
cp firmware/main/defaults.h firmware/main/button_gesture.h "$BUILD/"
for name in log_ring diag; do
    cc -std=c11 -Wall -Wextra -Werror -I"$BUILD" \
        "firmware/test/test_$name.c" "$BUILD/$name.c" -o "$BUILD/$name"
    "$BUILD/$name"
done
cc -std=c11 -Wall -Wextra -Werror -Ifirmware/test/log_stubs \
    -Ifirmware/test/epd_stubs -I"$BUILD" \
    firmware/test/test_log_capture.c "$BUILD/log_ring.c" "$BUILD/diag.c" -o "$BUILD/capture"
"$BUILD/capture"
