#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
BUILD=$(mktemp -d "${TMPDIR:-/tmp}/picpak-tests.XXXXXX")
trap 'rm -rf "$BUILD"' EXIT HUP INT TERM
IDF="${IDF_PATH:-$HOME/.platformio/packages/framework-espidf}"
MBEDTLS="$IDF/components/mbedtls/mbedtls"

for name in battpct lowbatt manual_mode rest_button maintenance_button; do
    cc -std=c11 -Wall -Wextra -Werror -Ifirmware/main \
        "firmware/test/test_$name.c" -lm -o "$BUILD/$name"
    "$BUILD/$name"
done
for name in fb2bpp provision_form mqtt_parse ble_photo; do
    cc -std=c11 -Wall -Wextra -Werror -Ifirmware/main \
        "firmware/test/test_$name.c" "firmware/main/$name.c" -o "$BUILD/$name"
    "$BUILD/$name"
done
cc -std=c11 -Wall -Wextra -Werror -Ifirmware/main -Ifirmware/main/vendor \
    firmware/test/test_maintenance.c firmware/main/maintenance_screen.c \
    firmware/main/fb2bpp.c firmware/main/vendor/qrcodegen.c -o "$BUILD/maintenance"
"$BUILD/maintenance" "${PICPAK_TEST_SCREEN:-$BUILD/maintenance.bin}"

mkdir "$BUILD/store-src"
# Compile the real store without a developer's optional secrets.h, so the
# fallback-credential regression fixture is deterministic and never reads secrets.
cp firmware/main/config_store.c firmware/main/config_store.h \
    firmware/main/defaults.h firmware/main/button_gesture.h "$BUILD/store-src/"
cc -std=c11 -Wall -Wextra -Werror -Ifirmware/test/stubs -I"$BUILD/store-src" \
    -DWIFI_DEFAULT_SSID='"compiled-network"' -DWIFI_DEFAULT_PASS='"compiled-password"' \
    firmware/test/test_maintenance_store.c "$BUILD/store-src/config_store.c" -o "$BUILD/store"
"$BUILD/store"

cc -std=c11 -Wall -Wextra -Werror \
    -DMBEDTLS_CONFIG_FILE='"relay_mbedtls_config.h"' \
    -Ifirmware/main -Ifirmware/test -I"$MBEDTLS/include" -I"$MBEDTLS/library" \
    firmware/test/test_ble_setup_protocol.c firmware/main/ble_setup_protocol.c \
    "$MBEDTLS/library/hkdf.c" "$MBEDTLS/library/md.c" \
    "$MBEDTLS/library/sha256.c" "$MBEDTLS/library/aes.c" \
    "$MBEDTLS/library/gcm.c" "$MBEDTLS/library/cipher.c" \
    "$MBEDTLS/library/cipher_wrap.c" "$MBEDTLS/library/platform_util.c" \
    "$MBEDTLS/library/constant_time.c" -o "$BUILD/ble_protocol"
"$BUILD/ble_protocol"
