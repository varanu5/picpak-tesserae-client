#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
BUILD=$(mktemp -d "${TMPDIR:-/tmp}/picpak-setup-tests.XXXXXX")
trap 'rm -rf "$BUILD"' EXIT HUP INT TERM
cp firmware/main/setup_identity.c firmware/main/setup_identity.h \
    firmware/main/defaults.h firmware/main/button_gesture.h "$BUILD/"
cc -std=c11 -Wall -Wextra -Werror \
    -I"$BUILD" -Ifirmware/test/transport_stubs -Ifirmware/test/epd_stubs \
    -Ifirmware/main -Ifirmware/main/vendor firmware/test/test_setup_identity.c \
    "$BUILD/setup_identity.c" firmware/main/setup_screen.c firmware/main/vendor/qrcodegen.c -o "$BUILD/setup"
"$BUILD/setup" firmware/main/assets/splash_setup.bin "${PICPAK_SETUP_SCREEN:-$BUILD/setup.bin}"
