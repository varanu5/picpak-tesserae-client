// provisioning.c — SoftAP captive portal for WiFi/server provisioning (M4).
// Ported (hybrid) from the Tesserae PhotoPainter reference src/provisioning.c;
// LAN/mDNS settings editor dropped, parsing delegated to provision_form, and
// persistence wired to config_store.
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "provisioning.h"
#include "provision_form.h"
#include "config_store.h"
#include "defaults.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "portal";

#define BIT_CREDS_SAVED  BIT0
static EventGroupHandle_t s_done;
static TaskHandle_t s_dns_task = NULL;
static httpd_handle_t s_httpd = NULL;
static esp_netif_t *s_ap_netif = NULL;

// Pre-scan cache: filled once at portal start, rendered into the form's <select>
// so the user can click a nearby SSID instead of typing.
#define SCAN_MAX PROVISION_SCAN_MAX
typedef struct {
    char    ssid[33];
    int8_t  rssi;
    bool    secure;
} scan_entry_t;
static scan_entry_t s_scan[SCAN_MAX];
static int          s_scan_count = 0;

// ---------- minimal wildcard DNS hijack ----------
// Listens on UDP/53 and answers every A query with our AP IP (192.168.4.1) so
// the phone's captive-portal probe pops our HTTP form automatically.
static void dns_hijack_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { ESP_LOGE(TAG, "dns sock fail"); vTaskDelete(NULL); }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(53),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "dns bind fail");
        close(sock); vTaskDelete(NULL);
    }

    const uint8_t our_ip[4] = {192, 168, 4, 1};

    uint8_t buf[512];
    while (1) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&src, &slen);
        if (n < 12) continue;   // DNS header is 12 bytes minimum

        buf[2] = 0x84;   // QR=1 AA=1 OPCODE=0
        buf[3] = 0x00;   // RCODE=0
        buf[6] = 0x00; buf[7] = 0x01;   // ANCOUNT
        buf[8] = 0x00; buf[9] = 0x00;   // NSCOUNT
        buf[10]= 0x00; buf[11]= 0x00;   // ARCOUNT

        int p = n;
        buf[p++] = 0xC0; buf[p++] = 0x0C;
        buf[p++] = 0x00; buf[p++] = 0x01;   // TYPE A
        buf[p++] = 0x00; buf[p++] = 0x01;   // CLASS IN
        buf[p++] = 0x00; buf[p++] = 0x00;   // TTL hi
        buf[p++] = 0x00; buf[p++] = 0x3C;   // TTL = 60s
        buf[p++] = 0x00; buf[p++] = 0x04;   // RDLENGTH
        for (int i = 0; i < 4; i++) buf[p++] = our_ip[i];

        sendto(sock, buf, p, 0, (struct sockaddr *)&src, slen);
    }
}

// ---------- HTML (inline; chunked-sent) ----------
static const char k_head[] =
"<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,viewport-fit=cover\">"
"<title>Tesserae Setup</title>"
"<style>"
":root{--bg:#f1f0ec;--surface:#fff;--fg:#18181b;--muted:#71706c;"
"--accent:#0d8c7e;--accent-hover:#0a6f63;--accent-soft:#e6f3f1;"
"--border:#e6e5e1;--radius:10px}"
"*{box-sizing:border-box}"
"body{margin:0;padding:24px 16px env(safe-area-inset-bottom);"
"font-family:-apple-system,BlinkMacSystemFont,\"Segoe UI\",Inter,system-ui,sans-serif;"
"font-size:15px;background:var(--bg);color:var(--fg);line-height:1.45;"
"-webkit-text-size-adjust:100%;-webkit-font-smoothing:antialiased}"
"main{max-width:480px;margin:0 auto}"
".brand{display:flex;align-items:center;gap:10px;margin:0 0 6px;"
"font-weight:700;font-size:20px;letter-spacing:-0.015em}"
".brand-mark{width:32px;height:32px;border-radius:8px;"
"background:linear-gradient(135deg,var(--accent),var(--accent-hover));"
"box-shadow:inset 0 1px 0 rgba(255,255,255,.35),"
"0 1px 3px rgba(13,140,126,.35),0 6px 16px rgba(13,140,126,.18)}"
".tag{color:var(--muted);font-size:13px;margin:0 0 18px}"
".card{background:var(--surface);border:1px solid var(--border);"
"border-radius:var(--radius);padding:18px 16px;margin-bottom:14px}"
".card h2{margin:0 0 14px;font-size:11px;text-transform:uppercase;"
"letter-spacing:0.08em;color:var(--muted);font-weight:600}"
".status{display:grid;grid-template-columns:auto 1fr;gap:6px 12px;"
"font-size:13px;color:var(--muted);margin-bottom:14px;padding:14px 16px;"
"background:var(--surface);border:1px solid var(--border);"
"border-radius:var(--radius)}"
".status .k{font-weight:600;color:var(--fg)}"
".status code{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;"
"font-size:12px;background:#fafaf9;padding:1px 6px;border-radius:4px;border:1px solid var(--border)}"
".field{margin-bottom:14px}"
".field:last-child{margin-bottom:0}"
"label{display:block;font-weight:500;font-size:13px;margin-bottom:6px;color:var(--fg)}"
"input:not([type=radio]):not([type=checkbox]),select{"
"width:100%;padding:10px 12px;border:1px solid var(--border);"
"border-radius:6px;background:var(--surface);font:inherit;font-size:15px;"
"color:var(--fg);-webkit-appearance:none;appearance:none;"
"transition:border-color .12s,box-shadow .12s}"
"input:focus,select:focus{outline:none;border-color:var(--accent);"
"box-shadow:0 0 0 3px rgba(13,140,126,.18)}"
"input[type=radio]{width:auto;margin:0 10px 0 0;vertical-align:middle;"
"accent-color:var(--accent);cursor:pointer}"
"label.radio{display:flex;align-items:flex-start;font-weight:500;"
"font-size:14px;margin-bottom:4px;cursor:pointer;color:var(--fg)}"
"label.radio strong{font-weight:600}"
"select{background-image:linear-gradient(45deg,transparent 50%,var(--muted) 50%),"
"linear-gradient(135deg,var(--muted) 50%,transparent 50%);"
"background-position:calc(100% - 16px) 50%,calc(100% - 11px) 50%;"
"background-size:5px 5px,5px 5px;background-repeat:no-repeat;padding-right:28px;cursor:pointer}"
".pw{position:relative}"
".pw input{padding-right:64px}"
".pw button{position:absolute;right:4px;top:50%;transform:translateY(-50%);"
"background:none;border:0;color:var(--accent);font:inherit;font-size:13px;"
"font-weight:600;padding:8px 10px;cursor:pointer;border-radius:4px}"
".pw button:hover{background:var(--accent-soft)}"
".hint{margin-top:6px;font-size:12px;color:var(--muted)}"
".hint code{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;"
"background:#fafaf9;padding:1px 5px;border-radius:3px;border:1px solid var(--border);font-size:11px}"
".err{background:#fef2f2;border:1px solid #fecaca;color:#b91c1c;"
"padding:12px 14px;border-radius:var(--radius);margin-bottom:14px;font-size:14px}"
"button.submit{width:100%;padding:12px 16px;border:0;border-radius:8px;"
"background:var(--accent);color:#fff;font:inherit;font-size:15px;"
"font-weight:600;cursor:pointer;margin-top:4px;"
"transition:background .12s}"
"button.submit:hover{background:var(--accent-hover)}"
"button.submit:active{transform:translateY(1px)}"
"</style></head><body><main>"
"<div class=\"brand\"><span class=\"brand-mark\"></span><span>Tesserae</span></div>"
"<p class=\"tag\">Device setup</p>";

// WiFi card; %s x2 = (ssid prefill, scan-picker HTML or empty)
static const char k_form_wifi_fmt[] =
"<form method=\"POST\" action=\"/save\">"
"<section class=\"card\"><h2>WiFi network</h2>"
"<div class=\"field\">"
"<label for=\"ssid\">Network name (SSID) *</label>"
"<input id=\"ssid\" name=\"ssid\" required maxlength=\"32\" "
"autocomplete=\"off\" value=\"%s\" placeholder=\"my-home-wifi\">"
"</div>"
"%s"
"<div class=\"field pw\">"
"<label for=\"wifi-pw\">Password</label>"
"<input id=\"wifi-pw\" name=\"pass\" type=\"password\" maxlength=\"64\" autocomplete=\"off\">"
"<button type=\"button\" data-toggle=\"wifi-pw\" aria-label=\"Show password\">Show</button>"
"<p class=\"hint\">Leave blank to keep the current password.</p>"
"</div>"
"</section>";

// Transport-mode card. MQTT is not implemented yet, so its radio is rendered
// disabled and REST is always pre-checked — no format args (see render_form).
// TODO(M5): restore the "%s x2 = (mqtt_checked, rest_checked)" pre-check when the
// MQTT client lands, and re-enable the radio below.
static const char k_form_transport[] =
"<section class=\"card\"><h2>Transport</h2>"
"<div class=\"field\">"
"<label class=\"radio\" for=\"tr-mqtt\" style=\"opacity:.5;cursor:not-allowed\">"
"<input type=\"radio\" name=\"transport\" value=\"mqtt\" id=\"tr-mqtt\" disabled>"
"<span><strong>MQTT broker</strong>"
"<p class=\"hint\" style=\"margin:2px 0 0\">"
"Subscribes to a retained frame topic. Requires a broker on your LAN. "
"(Coming soon &mdash; not yet selectable.)</p>"
"</span>"
"</label>"
"<label class=\"radio\" for=\"tr-rest\" style=\"margin-top:10px\">"
"<input type=\"radio\" name=\"transport\" value=\"rest\" id=\"tr-rest\" checked>"
"<span><strong>REST API</strong> <span style=\"color:var(--muted);font-weight:400\">(recommended)</span>"
"<p class=\"hint\" style=\"margin:2px 0 0\">"
"Polls the Tesserae server directly. No broker needed.</p>"
"</span>"
"</label>"
"</div>"
"</section>";

// Device + MQTT card; %s x3 = (device_id, mqtt_uri, mqtt_user).
static const char k_form_mqtt_fmt[] =
"<section class=\"card\"><h2>Device</h2>"
"<div class=\"field\">"
"<label for=\"device_id\">Device id <span style=\"color:var(--muted);font-weight:400\">(optional)</span></label>"
"<input id=\"device_id\" name=\"device_id\" maxlength=\"32\" "
"pattern=\"[a-z][a-z0-9_-]{1,31}\" autocomplete=\"off\" "
"value=\"%s\" placeholder=\"auto (picpak-xxxxxx)\">"
"<p class=\"hint\">Leave blank to auto-derive from the MAC.</p>"
"</div>"
"<div class=\"mqtt-only\">"
"<h2 style=\"margin-top:1.2em\">MQTT broker</h2>"
"<div class=\"field\">"
"<label for=\"mqtt_uri\">Broker URI</label>"
"<input id=\"mqtt_uri\" name=\"mqtt_uri\" maxlength=\"159\" "
"autocomplete=\"off\" value=\"%s\" placeholder=\"mqtt://192.168.1.50:1883\">"
"<p class=\"hint\">Use <code>mqtts://</code> for TLS; scheme is added if omitted.</p>"
"</div>"
"<div class=\"field\">"
"<label for=\"mqtt_user\">Username <span style=\"color:var(--muted);font-weight:400\">(optional)</span></label>"
"<input id=\"mqtt_user\" name=\"mqtt_user\" maxlength=\"63\" autocomplete=\"off\" value=\"%s\">"
"</div>"
"<div class=\"field pw\">"
"<label for=\"mqtt-pw\">Password <span style=\"color:var(--muted);font-weight:400\">(optional)</span></label>"
"<input id=\"mqtt-pw\" name=\"mqtt_pass\" type=\"password\" maxlength=\"63\" autocomplete=\"off\">"
"<button type=\"button\" data-toggle=\"mqtt-pw\" aria-label=\"Show password\">Show</button>"
"<p class=\"hint\">Leave blank to keep the current password.</p>"
"</div>"
"</div>"
"</section>";

// REST-API card; %s x2 = (server_url, pairing_code). Hidden by JS when transport=mqtt.
static const char k_form_rest_fmt[] =
"<section class=\"card rest-only\"><h2>Tesserae server</h2>"
"<div class=\"field\">"
"<label for=\"server_url\">Server URL</label>"
"<input id=\"server_url\" name=\"server_url\" maxlength=\"159\" "
"autocomplete=\"off\" value=\"%s\" placeholder=\"http://tesserae.local:8765\">"
"<p class=\"hint\">Where Tesserae is reachable from this network. "
"HTTP only in v1 (no TLS yet).</p>"
"</div>"
"<div class=\"field\">"
"<label for=\"pairing_code\">Pairing code <span style=\"color:var(--muted);font-weight:400\">(optional)</span></label>"
"<input id=\"pairing_code\" name=\"pairing_code\" maxlength=\"32\" "
"autocomplete=\"off\" value=\"%s\" placeholder=\"ABCDEF\">"
"<p class=\"hint\">Leave blank for friendly discovery (admin clicks "
"<em>Register</em> on Settings &rarr; Devices). Fill in to skip that "
"step using a code from <em>Pair new device</em>.</p>"
"</div>"
"</section>"
"<button class=\"submit\" type=\"submit\">Save &amp; restart</button>"
"</form>";

static const char k_tail[] =
"<script>"
"document.querySelectorAll('[data-toggle]').forEach(b=>{"
"b.addEventListener('click',()=>{"
"const i=document.getElementById(b.dataset.toggle);"
"const s=i.type==='password';"
"i.type=s?'text':'password';"
"b.textContent=s?'Hide':'Show';"
"});"
"});"
"const pick=document.getElementById('ssid-pick');"
"if(pick){pick.addEventListener('change',e=>{"
"if(e.target.value){document.getElementById('ssid').value=e.target.value;"
"document.getElementById('wifi-pw').focus();}"
"});}"
"function applyTransport(){"
"const m=document.getElementById('tr-mqtt').checked;"
"document.querySelectorAll('.mqtt-only').forEach(n=>n.style.display=m?'':'none');"
"document.querySelectorAll('.rest-only').forEach(n=>n.style.display=m?'none':'');"
"}"
"document.getElementById('tr-mqtt').addEventListener('change',applyTransport);"
"document.getElementById('tr-rest').addEventListener('change',applyTransport);"
"applyTransport();"
"</script></main></body></html>";

static const char k_thanks_html[] =
"<!doctype html><html><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>Saved</title>"
"<style>"
"body{margin:0;padding:40px 16px;background:#f1f0ec;color:#18181b;"
"font-family:-apple-system,BlinkMacSystemFont,system-ui,sans-serif;text-align:center}"
".card{max-width:380px;margin:0 auto;background:#fff;border:1px solid #e6e5e1;"
"border-radius:10px;padding:28px 20px}"
"h1{margin:0 0 8px;font-size:20px;color:#0d8c7e}"
"p{margin:0;color:#71706c;font-size:14px;line-height:1.5}"
"</style></head><body>"
"<div class=\"card\"><h1>Saved</h1>"
"<p>Tesserae will reboot and apply the new settings now.</p></div>"
"</body></html>";

// ---------- HTTP handlers ----------

// Render the settings form, pre-filled from config_store, with an optional error
// banner. Sent chunked so we don't need a multi-KB stack buffer. Transport
// defaults to REST (config_get_transport fallback) — our only working transport.
static esp_err_t render_form(httpd_req_t *req, const char *error)
{
    char server_url[160] = {0};
    config_get_server_url(server_url, sizeof server_url);
    char ssid[33] = {0}, pass[65] = {0};
    config_get_wifi(ssid, sizeof ssid, pass, sizeof pass);   // pass unused for the form

    char device_id[33] = {0};
    config_get_device_id(device_id, sizeof device_id);   // user-set or server-canonical

    char e_ssid[160], e_server[640], e_devid[100];
    provform_html_escape(ssid,       e_ssid,   sizeof e_ssid);
    provform_html_escape(server_url, e_server, sizeof e_server);
    provform_html_escape(device_id,  e_devid,  sizeof e_devid);
    // MQTT fields inert until M5; pairing code is single-use so never echoed back.
    const char *e_uri = "", *e_user = "", *e_pair = "";

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, k_head);

    if (error && *error) {
        httpd_resp_sendstr_chunk(req, "<div class=\"err\">");
        httpd_resp_sendstr_chunk(req, error);   // our own constant strings, safe
        httpd_resp_sendstr_chunk(req, "</div>");
    }

    httpd_resp_sendstr_chunk(req,
        "<div class=\"status\">"
        "<span class=\"k\">IP</span><span><code>192.168.4.1 (setup AP)</code></span>"
        "</div>");

    // Scan-picker <select> from cached scan results (empty if the scan found nothing).
    char picker[1024] = "";
    if (s_scan_count > 0) {
        size_t o = (size_t)snprintf(picker, sizeof picker,
            "<div class=\"field\">"
            "<label for=\"ssid-pick\">Or pick a nearby network</label>"
            "<select id=\"ssid-pick\" autocomplete=\"off\">"
            "<option value=\"\">-- pick a network --</option>");
        for (int i = 0; i < s_scan_count && o + 128 < sizeof picker; i++) {
            char esc[96];
            provform_html_escape(s_scan[i].ssid, esc, sizeof esc);
            int n = snprintf(picker + o, sizeof picker - o,
                "<option value=\"%s\">%s (%d dBm%s)</option>",
                esc, esc, s_scan[i].rssi, s_scan[i].secure ? "" : ", open");
            if (n < 0 || (size_t)n >= sizeof picker - o) break;
            o += (size_t)n;
        }
        if (o + 16 < sizeof picker) {
            snprintf(picker + o, sizeof picker - o, "</select></div>");
        }
    }

    char form_wifi[2048];
    snprintf(form_wifi, sizeof form_wifi, k_form_wifi_fmt, e_ssid, picker);
    httpd_resp_sendstr_chunk(req, form_wifi);

    // REST-only for now: the MQTT radio is disabled and REST is always checked,
    // so the stored transport no longer drives a pre-check. Send the static card.
    httpd_resp_sendstr_chunk(req, k_form_transport);

    char form_mqtt[1800];
    snprintf(form_mqtt, sizeof form_mqtt, k_form_mqtt_fmt, e_devid, e_uri, e_user);
    httpd_resp_sendstr_chunk(req, form_mqtt);

    char form_rest[1600];   // literal ~859B + escaped server_url (up to ~639B)
    snprintf(form_rest, sizeof form_rest, k_form_rest_fmt, e_server, e_pair);
    httpd_resp_sendstr_chunk(req, form_rest);

    httpd_resp_sendstr_chunk(req, k_tail);
    httpd_resp_sendstr_chunk(req, NULL);   // terminate chunked response
    return ESP_OK;
}

static esp_err_t h_root(httpd_req_t *req)
{
    return render_form(req, NULL);
}

// Parse the submitted form, validate, and persist via config_store. REST-default:
// transport is REST unless explicitly MQTT, so a normal save always stores
// server_url and feeds the validated REST client. Pairing state is NOT cleared —
// a server change self-heals via the 401->wipe in rest_handler.
static esp_err_t h_save(httpd_req_t *req)
{
    char body[1536]; int total = 0;
    while (total < (int)sizeof(body) - 1) {
        int n = httpd_req_recv(req, body + total, sizeof(body) - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    body[total] = '\0';

    char ssid[33] = {0}, pass[65] = {0}, transport[8] = {0};
    char mqtt_uri[160] = {0}, mqtt_user[64] = {0}, mqtt_pass[64] = {0};
    char server_url[192] = {0}, pairing_code[16] = {0}, device_id[33] = {0};

    bool have_ssid = provform_field(body, "ssid", ssid, sizeof ssid) && ssid[0];
    bool have_pass = provform_field(body, "pass", pass, sizeof pass) && pass[0];
    provform_field(body, "transport",   transport,   sizeof transport);
    bool have_uri  = provform_field(body, "mqtt_uri", mqtt_uri, sizeof mqtt_uri) && mqtt_uri[0];
    provform_field(body, "mqtt_user",  mqtt_user,   sizeof mqtt_user);
    bool have_mpw  = provform_field(body, "mqtt_pass", mqtt_pass, sizeof mqtt_pass) && mqtt_pass[0];
    provform_field(body, "server_url",  server_url,  sizeof server_url);
    provform_field(body, "pairing_code", pairing_code, sizeof pairing_code);
    bool have_devid = provform_field(body, "device_id", device_id, sizeof device_id) && device_id[0];

    // MQTT isn't implemented yet: the portal radio is disabled AND we force REST
    // here, so a stale NVS flag or a hand-crafted POST can't strand the device on
    // a transport with no client. TODO(M5): restore
    //   bool use_rest = (strcmp(transport, "mqtt") != 0);
    // once the MQTT client lands and re-enable the radio in k_form_transport.
    bool use_rest = true;
    uint8_t mode = use_rest ? 1 : 0;

    if (!have_ssid) return render_form(req, "WiFi network name (SSID) is required.");
    // Device id is optional (blank = auto-derive from MAC), but if given it must
    // match what the server accepts, else discovery would silently 400-loop.
    if (have_devid && !provform_device_id_valid(device_id))
        return render_form(req,
            "Device id must be lowercase, start with a letter, and use only "
            "letters, digits, - or _ (2-32 chars).");
    if (use_rest) {
        provform_url_result_t r = provform_normalize_server_url(server_url, sizeof server_url);
        if (r == PROVFORM_URL_EMPTY)
            return render_form(req, "Server URL is required when transport is REST.");
        if (r == PROVFORM_URL_BADSCHEME)
            return render_form(req, "Server URL must start with http:// or https://.");
    } else if (!have_uri) {
        return render_form(req, "MQTT broker URI is required when transport is MQTT.");
    }

    ESP_LOGI(TAG, "saving ssid='%s' transport=%s (req='%s') server='%s' device_id='%s'",
             ssid, use_rest ? "rest" : "mqtt", transport[0] ? transport : "rest",
             use_rest ? server_url : "(mqtt)", have_devid ? device_id : "(auto)");

    config_set_wifi(ssid, have_pass ? pass : NULL);   // blank pass keeps existing
    config_set_transport(mode);
    // Device id is shared across transports. Blank = leave as-is (auto-derive
    // from MAC at pair time, or keep the server's canonical id once paired).
    if (have_devid) config_set_device_id(device_id);
    if (use_rest) {
        config_set_server_url(server_url);
        config_set_pairing_code(pairing_code);
    } else {
        config_set_mqtt(mqtt_uri, mqtt_user, have_mpw ? mqtt_pass : NULL);
    }
    config_set_paired_pending(true);   // one-shot: forces a fresh repaint next boot

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, k_thanks_html, HTTPD_RESP_USE_STRLEN);
    xEventGroupSetBits(s_done, BIT_CREDS_SAVED);
    return ESP_OK;
}

// Captive-portal catch-all: redirect anything else to "/" so OS probes land on the form.
static esp_err_t h_catchall(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ---------- lifecycle helpers ----------

// Pre-AP scan: bring up STA briefly to populate s_scan[], then stop so start_ap()
// can take over the radio. Failures are non-fatal — the form still works.
static void do_wifi_scan(void)
{
    if (!esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) {
        esp_netif_create_default_wifi_sta();
    }

    s_scan_count = 0;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) { ESP_LOGW(TAG, "scan set_mode: %s", esp_err_to_name(err)); return; }
    err = esp_wifi_start();
    if (err != ESP_OK) { ESP_LOGW(TAG, "scan wifi_start: %s", esp_err_to_name(err)); return; }

    wifi_scan_config_t cfg = {0};
    err = esp_wifi_scan_start(&cfg, /* block */ true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan_start: %s", esp_err_to_name(err));
        esp_wifi_stop();
        return;
    }

    uint16_t n = SCAN_MAX;
    wifi_ap_record_t records[SCAN_MAX];
    esp_wifi_scan_get_ap_records(&n, records);

    for (int i = 0; i < (int)n && s_scan_count < SCAN_MAX; i++) {
        const char *ssid = (const char *)records[i].ssid;
        if (!ssid[0]) continue;   // skip hidden
        bool dup = false;
        for (int j = 0; j < s_scan_count; j++) {
            if (strcmp(s_scan[j].ssid, ssid) == 0) { dup = true; break; }
        }
        if (dup) continue;
        strncpy(s_scan[s_scan_count].ssid, ssid, 32);
        s_scan[s_scan_count].ssid[32] = '\0';
        s_scan[s_scan_count].rssi = records[i].rssi;
        s_scan[s_scan_count].secure = (records[i].authmode != WIFI_AUTH_OPEN);
        s_scan_count++;
    }
    ESP_LOGI(TAG, "scan: %d unique nearby networks", s_scan_count);

    esp_wifi_stop();
}

static void start_ap(void)
{
    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_config_t wc = {
        .ap = {
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)wc.ap.ssid,     PROVISION_AP_SSID, sizeof(wc.ap.ssid) - 1);
    strncpy((char *)wc.ap.password, PROVISION_AP_PASS, sizeof(wc.ap.password) - 1);
    wc.ap.ssid_len = strlen(PROVISION_AP_SSID);
    if (strlen(PROVISION_AP_PASS) < 8) wc.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    // PicPak brownout guard: cap TX power before the radio transmits (as in STA).
    esp_wifi_set_max_tx_power(WIFI_TX_POWER_QDBM);

    ESP_LOGI(TAG, "AP up: ssid=%s ip=192.168.4.1", PROVISION_AP_SSID);
}

static void start_http(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 6;
    cfg.stack_size = 16384;   // render_form holds several KB of locals at once
    // Phones flood the captive portal with parallel probe connections; without
    // this the small socket pool exhausts and accept() bounces (errno 23). LRU
    // purge recycles the oldest connection to accept a new one instead.
    cfg.lru_purge_enable = true;

    ESP_ERROR_CHECK(httpd_start(&s_httpd, &cfg));

    httpd_uri_t root = { .uri = "/",     .method = HTTP_GET,  .handler = h_root };
    httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = h_save };
    httpd_uri_t any  = { .uri = "/*",    .method = HTTP_GET,  .handler = h_catchall };
    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &save);
    httpd_register_uri_handler(s_httpd, &any);
}

// ---------- public API ----------

esp_err_t provisioning_run_blocking(void)
{
    s_done = xEventGroupCreate();

    // Self-contained WiFi base init: provisioning runs before wifi_start_sta and
    // always ends the boot (esp_restart on save, deep sleep on timeout), so this
    // never double-inits against wifi_manager in the same boot.
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t icfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&icfg));

    // Quick STA scan first so the form can offer a click-to-fill picker.
    do_wifi_scan();

    start_ap();
    start_http();
    xTaskCreate(dns_hijack_task, "dns_hijack", 4096, NULL, 5, &s_dns_task);

    ESP_LOGI(TAG, "captive portal up; waiting up to %ds for submission",
             PROVISION_PORTAL_TIMEOUT_S);
    EventBits_t bits = xEventGroupWaitBits(
        s_done, BIT_CREDS_SAVED, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(PROVISION_PORTAL_TIMEOUT_S * 1000));

    // Give the browser a beat to render the "saved" page before we tear AP down.
    vTaskDelay(pdMS_TO_TICKS(500));

    if (s_dns_task) { vTaskDelete(s_dns_task); s_dns_task = NULL; }
    if (s_httpd)    { httpd_stop(s_httpd);     s_httpd = NULL; }
    esp_wifi_stop();

    return (bits & BIT_CREDS_SAVED) ? ESP_OK : ESP_ERR_TIMEOUT;
}
