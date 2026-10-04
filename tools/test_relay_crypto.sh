#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
BUILD=$(mktemp -d "${TMPDIR:-/tmp}/picpak-crypto.XXXXXX")
trap 'rm -rf "$BUILD"' EXIT HUP INT TERM
IDF="${IDF_PATH:-$HOME/esp/esp-idf}"
MBEDTLS="$IDF/components/mbedtls/mbedtls"
JSON="$IDF/components/json/cJSON"
cc -std=c11 -Wall -Wextra -Werror \
    -DMBEDTLS_CONFIG_FILE='"relay_mbedtls_config.h"' \
    -Ifirmware/main -Ifirmware/main/vendor -Ifirmware/test \
    -I"$JSON" -I"$MBEDTLS/include" -I"$MBEDTLS/library" \
    firmware/test/test_relay_crypto.c firmware/main/relay_crypto.c firmware/main/relay_wire.c \
    firmware/main/vendor/monocypher.c "$JSON/cJSON.c" \
    "$MBEDTLS/library/hkdf.c" "$MBEDTLS/library/md.c" \
    "$MBEDTLS/library/sha256.c" "$MBEDTLS/library/aes.c" \
    "$MBEDTLS/library/gcm.c" "$MBEDTLS/library/cipher.c" \
    "$MBEDTLS/library/cipher_wrap.c" "$MBEDTLS/library/platform_util.c" \
    "$MBEDTLS/library/constant_time.c" -o "$BUILD/crypto"
"$BUILD/crypto"
