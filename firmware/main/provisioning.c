// provisioning.c — SoftAP captive portal for WiFi/server provisioning.
// Ported (hybrid) from the Tesserae PhotoPainter reference src/provisioning.c;
// LAN/mDNS settings editor dropped, parsing delegated to provision_form, and
// persistence wired to config_store.
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include "provisioning.h"
#include "provision_form.h"
#include "mqtt_parse.h"
#include "config_store.h"
#include "defaults.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_timer.h"
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
static volatile bool s_dns_stop = false;   // teardown -> task: exit and clean up
static httpd_handle_t s_httpd = NULL;
static esp_netif_t *s_ap_netif = NULL;

// AP-idle tracker: the portal timeout counts only while NO client is associated
// with the setup AP (reference behaviour) — a user filling the form can't be
// cut off mid-typing. 0 deadline = paused (client present).
static volatile int      s_ap_clients = 0;
static volatile int64_t  s_idle_deadline_us = 0;
static esp_event_handler_instance_t s_ap_evt_handle = NULL;

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
    if (sock < 0) { ESP_LOGE(TAG, "dns sock fail"); s_dns_task = NULL; vTaskDelete(NULL); }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(53),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "dns bind fail");
        close(sock); s_dns_task = NULL; vTaskDelete(NULL);
    }

    // Bounded recvfrom so the s_dns_stop flag is honoured: teardown flips it
    // and we close our own socket below, instead of being vTaskDelete'd while
    // blocked in recvfrom (which leaked the socket fd).
    struct timeval tv = { .tv_sec = 0, .tv_usec = 250 * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    const uint8_t our_ip[4] = {192, 168, 4, 1};

    uint8_t buf[512];
    while (!s_dns_stop) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&src, &slen);
        // DNS header is 12 bytes minimum; upper bound leaves room for the
        // 16-byte answer appended below (would overflow the stack otherwise)
        if (n < 12 || n > (int)sizeof(buf) - 16) continue;

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

    close(sock);
    s_dns_task = NULL;   // signals provisioning_run_blocking we're done
    vTaskDelete(NULL);
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
".brand-mark{width:32px;height:32px;flex:none;"
"filter:drop-shadow(0 2px 6px rgba(13,140,126,.28))}"
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
"input[type=radio]{width:18px;height:18px;flex:0 0 auto;margin:1px 12px 0 0;"
"accent-color:var(--accent);cursor:pointer}"
"label.radio{display:flex;align-items:flex-start;font-weight:500;"
"font-size:14px;margin:0;cursor:pointer;color:var(--fg);"
"padding:12px;border:1px solid var(--border);border-radius:8px;"
"transition:border-color .12s,background .12s}"
"label.radio+label.radio{margin-top:10px}"
"label.radio:has(input:checked){border-color:var(--accent);"
"background:var(--accent-soft)}"
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
".credits{margin:20px 4px 6px;text-align:center;font-size:11.5px;"
"color:var(--muted);line-height:1.8}"
".credits span{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:11px}"
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
/* Brand mark: the exact tesserae/static/brand/icon.svg (rounded-square gradient
 * + two white 0.85-alpha quadrants clipped to an inner rounded rect). */
"<div class=\"brand\">"
"<svg class=\"brand-mark\" viewBox=\"0 0 256 256\" width=\"32\" height=\"32\" aria-hidden=\"true\">"
"<defs>"
"<linearGradient id=\"tess-bg\" x1=\"0\" y1=\"0\" x2=\"1\" y2=\"1\">"
"<stop offset=\"0\" stop-color=\"#0d8c7e\"/><stop offset=\"1\" stop-color=\"#0a6f63\"/>"
"</linearGradient>"
"<clipPath id=\"tess-inner\"><rect x=\"55\" y=\"55\" width=\"146\" height=\"146\" rx=\"27\" ry=\"27\"/></clipPath>"
"</defs>"
"<rect x=\"0\" y=\"0\" width=\"256\" height=\"256\" rx=\"72\" ry=\"72\" fill=\"url(#tess-bg)\"/>"
"<g clip-path=\"url(#tess-inner)\" fill=\"#ffffff\" fill-opacity=\"0.85\">"
"<rect x=\"128\" y=\"55\" width=\"73\" height=\"73\"/>"
"<rect x=\"55\" y=\"128\" width=\"73\" height=\"73\"/>"
"</g>"
"</svg>"
"<span>Tesserae</span></div>"
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

// Transport-mode card; %s x2 = (mqtt_checked, rest_checked) — pre-checked from
// the stored transport so re-provisioning reopens on the current mode.
static const char k_form_transport_fmt[] =
"<section class=\"card\"><h2>Transport</h2>"
"<div class=\"field\">"
"<label class=\"radio\" for=\"tr-mqtt\">"
"<input type=\"radio\" name=\"transport\" value=\"mqtt\" id=\"tr-mqtt\"%s>"
"<span><strong>MQTT broker</strong>"
"<p class=\"hint\" style=\"margin:2px 0 0\">"
"Subscribes to a retained frame topic. Requires a broker on your LAN "
"(Mosquitto or Tesserae&rsquo;s embedded broker).</p>"
"</span>"
"</label>"
"<label class=\"radio\" for=\"tr-rest\">"
"<input type=\"radio\" name=\"transport\" value=\"rest\" id=\"tr-rest\"%s>"
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
"<p class=\"hint\">Where Tesserae is reachable from this network &mdash; "
"hostname or LAN IP (e.g. <code>http://192.168.1.100:8765</code>). "
"<code>https://</code> needs a publicly-trusted certificate; "
"self-signed won&rsquo;t work.</p>"
"</div>"
"<div class=\"field\">"
"<label for=\"pairing_code\">Pairing code <span style=\"color:var(--muted);font-weight:400\">(optional)</span></label>"
"<input id=\"pairing_code\" name=\"pairing_code\" maxlength=\"32\" "
"autocomplete=\"off\" value=\"%s\">"
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
"</script>"
// Credits footer. Plain text, not links: the captive portal's DNS hijack
// redirects every URL back to this form, so an anchor would only reload the
// page. GitHub repo slugs (author/repo) keep the lines short.
"<div class=\"credits\">"
"Dashboard companion &middot; <span>dmellok/tesserae</span><br>"
"Firmware &middot; <span>varanu5/picpak-tesserae-client</span>"
"</div>"
"</main></body></html>";

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

    uint8_t transport = config_get_transport(1);   // 0=MQTT, 1=REST
    char mqtt_uri[160] = {0}, mqtt_user[64] = {0}, mqtt_pass[64] = {0};
    config_get_mqtt(mqtt_uri, sizeof mqtt_uri, mqtt_user, sizeof mqtt_user,
                    mqtt_pass, sizeof mqtt_pass);   // pass unused for the form

    // e_server sized for the worst case: 159-char URL fully &quot;-escaped
    // (159 x 6 = 954 + NUL) — 640 used to display a chopped URL in the form.
    char e_ssid[160], e_server[960], e_devid[100];
    provform_html_escape(ssid,       e_ssid,   sizeof e_ssid);
    provform_html_escape(server_url, e_server, sizeof e_server);
    provform_html_escape(device_id,  e_devid,  sizeof e_devid);
    // Broker URI worst case: 159 chars fully &quot;-escaped = 954 + NUL.
    // Pairing code is single-use so never echoed back.
    char e_uri[960], e_user[384];
    provform_html_escape(mqtt_uri,  e_uri,  sizeof e_uri);
    provform_html_escape(mqtt_user, e_user, sizeof e_user);
    const char *e_pair = "";

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

    char form_transport[sizeof k_form_transport_fmt + 32];
    snprintf(form_transport, sizeof form_transport, k_form_transport_fmt,
             transport == 0 ? " checked" : "", transport == 0 ? "" : " checked");
    httpd_resp_sendstr_chunk(req, form_transport);

    // Literal ~1.5 KB + e_devid(100) + e_uri(960) + e_user(384). The httpd task
    // runs on a 16 KB stack (see cfg.stack_size below) sized for these locals.
    char form_mqtt[3072];
    snprintf(form_mqtt, sizeof form_mqtt, k_form_mqtt_fmt, e_devid, e_uri, e_user);
    httpd_resp_sendstr_chunk(req, form_mqtt);

    char form_rest[2048];   // literal ~970B (incl. https hint) + escaped server_url (up to ~959B)
    snprintf(form_rest, sizeof form_rest, k_form_rest_fmt, e_server, e_pair);
    httpd_resp_sendstr_chunk(req, form_rest);

    httpd_resp_sendstr_chunk(req, k_tail);
    httpd_resp_sendstr_chunk(req, NULL);   // terminate chunked response
    return ESP_OK;
}

// Initial banner for re-entry portals ("server unreachable" etc.); shown on
// every GET until a save. Always one of our own constant strings — safe HTML.
static const char *s_initial_note = NULL;

static esp_err_t h_root(httpd_req_t *req)
{
    return render_form(req, s_initial_note);
}

// Parse the submitted form, validate, and persist via config_store. REST-default:
// transport is REST unless explicitly MQTT, so a normal save always stores
// server_url and feeds the validated REST client. Pairing state is NOT cleared —
// a server change self-heals via the 401->wipe in rest_handler.
static esp_err_t h_save(httpd_req_t *req)
{
    // static: keeps 3 KB off the httpd task stack; safe because ESP-IDF httpd runs
    // all handlers on one task. 3072 covers the worst-case fully percent-encoded
    // form (~1.9 KB incl. the MQTT fields, which still submit while JS-hidden).
    static char body[3072];
    int total = 0;
    // A body that wouldn't fit would parse as silently truncated fields (e.g. a
    // chopped server_url persisted without error) — reject it outright instead.
    if (req->content_len >= sizeof(body)) {
        // Drain and discard so the leftover bytes aren't misparsed as a next
        // request on this connection, then re-render the form with the error.
        char sink[256];
        int left = (int)req->content_len;
        while (left > 0) {
            int n = httpd_req_recv(req, sink, left < (int)sizeof(sink) ? left : (int)sizeof(sink));
            if (n <= 0) break;
            left -= n;
        }
        return render_form(req, "Submission too large. Please shorten the longest fields and try again.");
    }
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

    // REST unless explicitly MQTT, so a malformed/absent field still lands on
    // the recommended transport.
    bool use_rest = (strcmp(transport, "mqtt") != 0);
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
        mqtt_normalize_uri(mqtt_uri, sizeof mqtt_uri);   // "host:1883" -> "mqtt://host:1883"
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

// ---------- AP-idle tracker ----------

// SoftAP join/leave events drive the idle deadline: any associated client
// pauses the countdown; the last one leaving restarts the full window.
static void on_ap_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base != WIFI_EVENT) return;

    if (id == WIFI_EVENT_AP_STACONNECTED) {
        s_ap_clients++;
        s_idle_deadline_us = 0;   // clients present -> no idle timeout
        ESP_LOGI(TAG, "AP client joined (clients=%d); idle timer paused", s_ap_clients);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_ap_clients > 0) s_ap_clients--;
        if (s_ap_clients == 0) {
            s_idle_deadline_us = esp_timer_get_time() +
                (int64_t)PROVISION_PORTAL_TIMEOUT_S * 1000000LL;
            ESP_LOGI(TAG, "AP client left (none connected); idle countdown restarted (%ds)",
                     PROVISION_PORTAL_TIMEOUT_S);
        } else {
            ESP_LOGI(TAG, "AP client left (clients=%d)", s_ap_clients);
        }
    }
}

static void idle_tracker_install(void)
{
    s_ap_clients = 0;
    s_idle_deadline_us = esp_timer_get_time() +
        (int64_t)PROVISION_PORTAL_TIMEOUT_S * 1000000LL;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_ap_event, NULL, &s_ap_evt_handle));
}

static void idle_tracker_uninstall(void)
{
    if (s_ap_evt_handle) {
        esp_event_handler_instance_unregister(
            WIFI_EVENT, ESP_EVENT_ANY_ID, s_ap_evt_handle);
        s_ap_evt_handle = NULL;
    }
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

    // esp_wifi_start() is asynchronous, and on the banner-path re-entry the
    // radio may still be settling from the failed connect's teardown — either
    // way scan_start can transiently return ESP_ERR_WIFI_STATE. Retry briefly
    // (~1.5 s cap) instead of giving up and rendering an empty picker.
    wifi_scan_config_t cfg = {0};
    err = ESP_ERR_WIFI_STATE;
    for (int attempt = 0; attempt < 15 && err == ESP_ERR_WIFI_STATE; attempt++) {
        err = esp_wifi_scan_start(&cfg, /* block */ true);
        if (err == ESP_ERR_WIFI_STATE) vTaskDelay(pdMS_TO_TICKS(100));
    }
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

esp_err_t provisioning_run_blocking(const char *note)
{
    s_initial_note = note;
    s_done = xEventGroupCreate();

    // WiFi base init, tolerant of a prior wifi_start_sta in the same boot (the
    // server-unreachable re-entry path): netif/event-loop report INVALID_STATE
    // when already up, and esp_wifi_init is a no-op ESP_OK when already inited.
    // Note: wifi_manager's STA event handler stays registered in that case; its
    // STA_START->connect can make the pre-AP scan come up empty (non-fatal, the
    // SSID field is prefilled anyway).
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err);
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err);
    wifi_init_config_t icfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&icfg));

    // Quick STA scan first so the form can offer a click-to-fill picker.
    do_wifi_scan();

    start_ap();
    idle_tracker_install();
    start_http();
    s_dns_stop = false;
    xTaskCreate(dns_hijack_task, "dns_hijack", 4096, NULL, 5, &s_dns_task);

    ESP_LOGI(TAG, "captive portal up; %ds idle timeout (paused while a client is connected)",
             PROVISION_PORTAL_TIMEOUT_S);
    // Poll in 1 s chunks: react to a save OR the idle deadline elapsing with no
    // client associated. The deadline is zeroed while >=1 client is connected,
    // so a user actively on the form can never be cut off.
    EventBits_t bits = 0;
    while (1) {
        bits = xEventGroupWaitBits(s_done, BIT_CREDS_SAVED,
                                   pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));
        if (bits & BIT_CREDS_SAVED) break;
        if (s_idle_deadline_us != 0 && esp_timer_get_time() > s_idle_deadline_us) {
            ESP_LOGW(TAG, "captive portal idle for %ds with no client; giving up",
                     PROVISION_PORTAL_TIMEOUT_S);
            break;
        }
    }

    // Give the browser a beat to render the "saved" page before we tear AP down.
    vTaskDelay(pdMS_TO_TICKS(500));

    idle_tracker_uninstall();

    // Cooperative DNS-task shutdown: the task closes its own socket and clears
    // s_dns_task (a vTaskDelete from here mid-recvfrom leaked the fd — harmless
    // while the portal ends every boot, a real leak if it ever goes re-entrant).
    // Worst-case wait is one 250 ms recv timeout; hard-kill kept as a fallback.
    if (s_dns_task) {
        s_dns_stop = true;
        for (int i = 0; i < 50 && s_dns_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
        if (s_dns_task) { vTaskDelete(s_dns_task); s_dns_task = NULL; }
    }
    if (s_httpd)    { httpd_stop(s_httpd);     s_httpd = NULL; }
    esp_wifi_stop();

    return (bits & BIT_CREDS_SAVED) ? ESP_OK : ESP_ERR_TIMEOUT;
}
