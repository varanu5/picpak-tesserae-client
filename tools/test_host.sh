#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
BUILD=$(mktemp -d "${TMPDIR:-/tmp}/picpak-tests.XXXXXX")
trap 'rm -rf "$BUILD"' EXIT HUP INT TERM
IDF="${IDF_PATH:-$HOME/.platformio/packages/framework-espidf}"
MBEDTLS="$IDF/components/mbedtls/mbedtls"

sh tools/test_battery_wake.sh
sh tools/test_device_logs.sh
sh tools/test_setup_identity.sh
sh tools/test_wake_align.sh
sh tools/test_transport_setup.sh
sh tools/test_relay_crypto.sh

cc -std=c11 -Wall -Wextra -Werror -Ifirmware/main \
    firmware/test/test_network_safety.c -o "$BUILD/network"
"$BUILD/network"
cc -std=c11 -Wall -Wextra -Werror \
    -Ifirmware/test/transport_stubs -Ifirmware/test/epd_stubs -Ifirmware/main \
    firmware/test/test_image_fetcher.c firmware/main/image_fetcher.c -o "$BUILD/image"
"$BUILD/image"

# Copy only display sources and public configuration headers; do not load local secrets.
mkdir "$BUILD/epd-src"
cp firmware/main/epd_driver.c firmware/main/epd_driver.h firmware/main/epd_init_seq.h \
    firmware/main/epd_lut_5s.h firmware/main/epd_lut_10s.h firmware/main/board.h \
    firmware/main/config_store.h firmware/main/defaults.h firmware/main/button_gesture.h \
    firmware/main/log_capture.h firmware/main/diag.h \
    "$BUILD/epd-src/"
cc -std=c11 -Wall -Wextra -Werror -Ifirmware/test/epd_stubs -I"$BUILD/epd-src" \
    firmware/test/test_epd_driver.c "$BUILD/epd-src/epd_driver.c" -o "$BUILD/epd"
"$BUILD/epd" firmware/test/fixtures

for name in battpct lowbatt manual_mode rest_button maintenance_button connection_retry; do
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
    firmware/main/defaults.h firmware/main/button_gesture.h firmware/main/setup_check.h "$BUILD/store-src/"
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
