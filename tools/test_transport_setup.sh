#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
BUILD=$(mktemp -d "${TMPDIR:-/tmp}/picpak-transport-tests.XXXXXX")
trap 'rm -rf "$BUILD"' EXIT HUP INT TERM
IDF="${IDF_PATH:-$HOME/esp/esp-idf}"
JSON="$IDF/components/json/cJSON"

# Copy public sources without the optional credentials header.
for name in rest_handler relay relay_wire relay_crypto mqtt_handler mqtt_parse; do
    cp "firmware/main/$name.c" "firmware/main/$name.h" "$BUILD/"
done
for name in config_store defaults rest_button image_fetcher framebuf heartbeat board \
            wake_align setup_check connection_retry button_gesture http_redirect wifi_manager power log_capture diag; do
    cp "firmware/main/$name.h" "$BUILD/"
done
cc -std=c11 -Wall -Wextra -Werror -DTEST_REST \
    -Ifirmware/test/transport_stubs -Ifirmware/test/epd_stubs -I"$BUILD" -I"$JSON" \
    firmware/test/test_transport_setup.c "$JSON/cJSON.c" -o "$BUILD/rest"
"$BUILD/rest"
cc -std=c11 -Wall -Wextra -Werror -DTEST_RELAY -DESP_PLATFORM \
    -Ifirmware/test/transport_stubs -Ifirmware/test/epd_stubs -I"$BUILD" -I"$JSON" \
    firmware/test/test_transport_setup.c "$BUILD/relay_wire.c" "$JSON/cJSON.c" -o "$BUILD/relay"
"$BUILD/relay"
cc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -DTEST_MQTT \
    -Ifirmware/test/transport_stubs -Ifirmware/test/epd_stubs -I"$BUILD" \
    firmware/test/test_transport_setup.c "$BUILD/mqtt_parse.c" -o "$BUILD/mqtt"
"$BUILD/mqtt"
