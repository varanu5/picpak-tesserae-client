// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include "setup_request.h"
#include "defaults.h"
#include "config_store.h"
#include "mqtt_parse.h"
#include "provision_form.h"
#include "wifi_credentials.h"
#include <string.h>

static void trim_code(char *value) {
    size_t start = strspn(value, " \t\r\n");
    if (start) memmove(value, value + start, strlen(value + start) + 1);
    size_t end = strlen(value);
    while (end && strchr(" \t\r\n", value[end - 1])) value[--end] = 0;
}

static bool cloud_claim(char *value) {
    size_t count = 0;
    for (size_t i = 0; value[i]; i++) {
        if (value[i] == ' ') continue;
        if (value[i] < '0' || value[i] > '9' || count == 8) return false;
        value[count++] = value[i];
    }
    value[count] = 0;
    return count == 8;
}

const char *setup_display_mode_parse(const char *body, const char *nonce, display_mode_t *out) {
    char received[33], mode[16];
    unsigned nonces = 0, modes = 0;
    for (const char *p = body; p && *p;) {
        if (!strncmp(p, "nonce=", 6)) nonces++;
        if (!strncmp(p, "waveform=", 9)) modes++;
        p = strchr(p, '&');
        if (p) p++;
    }
    if (nonces != 1 || modes != 1) return "Reload the page and choose one refresh mode.";
    if (provform_parse_field(body, "nonce", received, sizeof received) != PROVFORM_FIELD_OK ||
        strcmp(received, nonce)) return "Setup expired. Reload this page and try again.";
    if (provform_parse_field(body, "waveform", mode, sizeof mode) != PROVFORM_FIELD_OK)
        return "Choose a display refresh mode.";
    if (!strcmp(mode, "native")) *out = DISPLAY_NATIVE;
    else if (!strcmp(mode, "5s")) *out = DISPLAY_FAST_5S;
    else if (!strcmp(mode, "10s")) *out = DISPLAY_FAST_10S;
    else return "Unknown display refresh mode.";
    return NULL;
}

const char *setup_request_parse(const char *body, const char *nonce,
                               const char *old_ssid, const char *old_pass,
                               const char *old_id, setup_request_t *out) {
    char received[33], obsolete_open[2];
    memset(out, 0, sizeof(*out));
    if (provform_parse_field(body, "nonce", received, sizeof received) != PROVFORM_FIELD_OK ||
        strcmp(received, nonce)) return "Setup expired. Reload this page and try again.";
    char transport[16];
    int transport_result = provform_parse_field(body, "transport", transport, sizeof transport);
    if (transport_result == PROVFORM_FIELD_INVALID ||
        (transport[0] && strcmp(transport, "rest") && strcmp(transport, "cloud") && strcmp(transport, "relay") && strcmp(transport, "mqtt")))
        return "Choose Local REST, Cloud, Relay or MQTT.";
    out->relay = !strcmp(transport, "relay");
    out->cloud = !strcmp(transport, "cloud");
    out->mqtt = !strcmp(transport, "mqtt");
    bool local = !out->relay && !out->cloud && !out->mqtt;
    const struct { const char *name; char *value; size_t cap; bool active; } fields[] = {
        {"ssid", out->ssid, sizeof out->ssid, true}, {"pass", out->pass, sizeof out->pass, true},
        {"server", out->server, sizeof out->server, local},
        {out->cloud ? "cloud_id" : out->mqtt ? "mqtt_id" : "id", out->id, sizeof out->id, !out->relay},
        {out->cloud ? "cloud_code" : "pair", out->pair, sizeof out->pair, !out->relay && !out->mqtt},
        {"mqtt_uri", out->mqtt_uri, sizeof out->mqtt_uri, out->mqtt},
        {"mqtt_user", out->mqtt_user, sizeof out->mqtt_user, out->mqtt},
        {"mqtt_pass", out->mqtt_pass, sizeof out->mqtt_pass, out->mqtt},
        {"relay_url", out->relay_url, sizeof out->relay_url, out->relay},
        {"relay_code", out->relay_code, sizeof out->relay_code, out->relay},
    };
    for (unsigned i=0; i<sizeof(fields)/sizeof(fields[0]); i++)
        if (fields[i].active &&
            provform_parse_field(body, fields[i].name, fields[i].value, fields[i].cap) == PROVFORM_FIELD_INVALID)
            return "A field is too long or has invalid encoding.";
    char waveform[16];
    if (provform_parse_field(body, "waveform", waveform, sizeof waveform) != PROVFORM_FIELD_MISSING) {
        const char *error = setup_display_mode_parse(body, nonce, &out->display_mode);
        if (error) return error;
        out->has_display_mode = true;
    }
    if (provform_parse_field(body, "open", obsolete_open, sizeof obsolete_open) != PROVFORM_FIELD_MISSING)
        return "Open networks are not supported. Reload setup and use a password-protected network.";
    if (!out->pass[0] && !strcmp(out->ssid, old_ssid) && strlen(old_pass)<sizeof out->pass)
        strcpy(out->pass, old_pass);
    if (!wifi_credentials_valid(out->ssid, out->pass))
        return "Enter a network name up to 32 bytes and an 8-63 byte password, or a 64-digit hex key.";
    if (!out->relay && out->id[0] && !provform_device_id_valid(out->id))
        return "Device ID needs 2-32 lowercase letters, digits, hyphens or underscores, starting with a letter.";
    if (!out->id[0]) {
        if (strlen(old_id)>=sizeof out->id) return "Stored device ID is too long. Enter a shorter ID.";
        strcpy(out->id, old_id);
    }
    trim_code(out->pair);
    trim_code(out->relay_code);
    if (out->mqtt) {
        if (!out->mqtt_uri[0]) return "Enter the MQTT broker address.";
        if (strstr(out->mqtt_uri, "://") && strncmp(out->mqtt_uri,"mqtt://",7) &&
            strncmp(out->mqtt_uri,"mqtts://",8) && strncmp(out->mqtt_uri,"ws://",5) && strncmp(out->mqtt_uri,"wss://",6))
            return "Use mqtt://, mqtts://, ws:// or wss:// for the broker.";
        mqtt_normalize_uri(out->mqtt_uri, sizeof out->mqtt_uri);
        if (strncmp(out->mqtt_uri,"mqtt://",7) && strncmp(out->mqtt_uri,"mqtts://",8) &&
            strncmp(out->mqtt_uri,"ws://",5) && strncmp(out->mqtt_uri,"wss://",6))
            return "Use mqtt://, mqtts://, ws:// or wss:// for the broker.";
        const char *host = strstr(out->mqtt_uri, "://") + 3;
        if (!*host || *host == '/' || strpbrk(out->mqtt_uri, " \t\r\n?#@\\\"<>"))
            return "Enter a broker address without embedded credentials, query or fragment.";
        return NULL;
    }
    if (out->cloud) {
        if (out->pair[0] && !cloud_claim(out->pair)) return "Enter the eight-digit claim code from Tesserae Cloud.";
        strcpy(out->server, SETUP_CLOUD_URL);
        return NULL;
    }
    if (out->relay) {
        if (strlen(out->relay_code) > 32 || strspn(out->relay_code,
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-") != strlen(out->relay_code))
            return "Enter the cloud relay pairing code shown by your server.";
        if (provform_normalize_server_url(out->relay_url, sizeof out->relay_url) != PROVFORM_URL_OK)
            return "Enter an HTTPS relay address.";
        const char *host = strstr(out->relay_url, "://") + 3;
        if (!*host || *host == '/' || strpbrk(out->relay_url, " \t\r\n?#@\\\"<>"))
            return "Enter a relay address without credentials, query or fragment.";
        if (strncmp(out->relay_url, "https://", 8)) return "The relay address must start with https://.";
        return NULL;
    }
    if (out->pair[0] && (strlen(out->pair)!=6 || strspn(out->pair,"0123456789")!=6))
        return "Pairing code must contain six digits.";
    if (provform_normalize_server_url(out->server, sizeof out->server) != PROVFORM_URL_OK)
        return "Enter an HTTP or HTTPS server address.";
    if (!strcmp(out->server, SETUP_CLOUD_URL)) return "Select Cloud for the Tesserae Cloud address.";
    const char *host = strstr(out->server, "://") + 3;
    if (!*host || *host=='/' || strpbrk(out->server," \t\r\n?#@\\\"<>"))
        return "Enter a server address without credentials, query or fragment.";
    return NULL;
}

const char *setup_request_check_cloud_pairing(const setup_request_t *request,
    const char *old_server, const char *old_id, bool paired) {
    if (!request->cloud || request->pair[0]) return NULL;
    if (!paired || strcmp(old_server, SETUP_CLOUD_URL) || strcmp(request->id, old_id))
        return "Enter a new eight-digit Cloud claim code for first pairing or a changed Device ID.";
    return NULL;
}
