// provisioning.c — SoftAP captive portal for WiFi/server provisioning.
// Ported (hybrid) from the Tesserae PhotoPainter reference src/provisioning.c;
// LAN/mDNS settings editor dropped, parsing delegated to provision_form, and
// persistence wired to config_store.
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include "provisioning.h"
#include "setup_identity.h"
#include "setup_request.h"
#include "portal_view.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "provision_form.h"
#include "wifi_credentials.h"
#include "mqtt_parse.h"
#include "config_store.h"
#include "epd_driver.h"   // epd_waveform_t (portal <-> driver value contract)
#include "relay.h"        // RELAY_DEFAULT_URL
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

static const char k_portal_html[] =
"<!doctype html>\n"
"<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->\n"
"<!-- Copyright (C) 2026 varanu5 <https://github.com/varanu5> -->\n"
"<html lang=\"en\">\n"
"<head>\n"
"<meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,viewport-fit=cover\">\n"
"<title>PicPak setup</title>\n"
"<style>\n"
":root{color-scheme:light;--bg:#f1f0ec;--surface:#fff;--ink:#18181b;--muted:#62625f;--accent:#0d8c7e;--accent-hover:#0a6f63;--soft:#e6f3f1;--border:#deded8}\n"
"*{box-sizing:border-box}\n"
"body{margin:0;padding:24px 16px max(28px,env(safe-area-inset-bottom));background:var(--bg);color:var(--ink);font:15px/1.45 -apple-system,BlinkMacSystemFont,\"Segoe UI\",system-ui,sans-serif;-webkit-text-size-adjust:100%}\n"
"main{max-width:480px;margin:auto}\n"
".brand{display:flex;gap:10px;align-items:center;font-size:21px;font-weight:700;letter-spacing:-.02em}\n"
".brand svg{flex:none;width:34px;height:34px}\n"
"h1{font-size:14px;font-weight:400;color:var(--muted);margin:6px 0 18px}\n"
"p{margin:0}.hint,.muted{color:var(--muted)}.hint{font-size:12px;line-height:1.5;margin-top:6px}\n"
".status{display:grid;grid-template-columns:auto 1fr;gap:7px 14px;font-size:12px;border:1px solid var(--border);border-radius:10px;background:var(--surface);padding:14px 16px;margin:0 0 14px}\n"
".status dt{font-weight:600}.status dd{margin:0;color:var(--muted);overflow-wrap:anywhere}.status code{font-size:12px}\n"
".card{border:1px solid var(--border);border-radius:12px;background:var(--surface);padding:18px 16px;margin-bottom:14px}\n"
"[hidden]{display:none!important}\n"
".card h2{font-size:11px;font-weight:600;letter-spacing:.08em;text-transform:uppercase;color:var(--muted);margin:0 0 14px}\n"
".field{margin-bottom:16px}.field:last-child{margin-bottom:0}\n"
"label{display:block;font-weight:600;font-size:13px;margin-bottom:6px}\n"
"input:not([type=checkbox]):not([type=radio]),select{width:100%;min-height:44px;border:1px solid var(--border);border-radius:7px;background:var(--surface);padding:10px 12px;color:var(--ink);font:inherit;font-size:16px;line-height:1.3}\n"
"input:focus-visible,select:focus-visible,button:focus-visible{outline:3px solid #a8d8d0;outline-offset:2px}\n"
"#relay-url::placeholder{color:var(--muted);opacity:1}\n"
"input:disabled{background:#f5f5f2;color:var(--muted)}\n"
".password{position:relative}.password input{padding-right:66px!important}.password button{position:absolute;right:3px;top:3px;min-height:38px;padding:0 12px;border:0;border-radius:5px;background:transparent;color:var(--accent);font:inherit;font-size:13px;font-weight:600;cursor:pointer}\n"
".password button:hover{background:var(--soft)}\n"
".check{display:flex;align-items:center;gap:8px;font-size:12px;font-weight:400;margin:10px 0 0}.check input{flex:none;width:18px;height:18px;margin:0;accent-color:var(--accent)}\n"
"fieldset{border:0;margin:0;padding:0;min-width:0}\n"
".radio{display:flex;align-items:flex-start;gap:12px;padding:12px;border:1px solid var(--border);border-radius:8px;margin:0 0 9px;cursor:pointer;font-weight:400}\n"
".radio:last-child{margin-bottom:0}.radio input{flex:none;width:18px;height:18px;margin:2px 0 0;accent-color:var(--accent)}\n"
".radio:has(input:checked){background:var(--soft);border-color:var(--accent)}\n"
".radio strong{font-size:14px;font-weight:600}.radio .hint{display:block;margin-top:3px}\n"
".badge{display:inline-block;vertical-align:1px;font-size:10px;font-weight:500;color:var(--muted);border:1px solid var(--border);border-radius:4px;padding:1px 5px;margin-left:5px}\n"
".refresh-help{margin-top:12px}.refresh-help p+p{margin-top:6px}\n"
".submit{width:100%;min-height:48px;border:0;border-radius:8px;padding:12px 16px;background:var(--accent);color:white;font:inherit;font-size:15px;font-weight:600;cursor:pointer}\n"
".submit:hover{background:var(--accent-hover)}button:disabled{opacity:.6;cursor:wait}\n"
".notice{padding:12px 14px;background:#fff8dd;border:1px solid #dec781;border-radius:10px;font-size:13px;margin-bottom:14px;overflow-wrap:anywhere}.notice:empty{display:none}\n"
"#progress{padding:2px 4px}footer{text-align:center;color:var(--muted);font-size:11px;line-height:1.8;margin:22px 0 0}footer span{font-family:ui-monospace,SFMono-Regular,Menlo,monospace}code{overflow-wrap:anywhere}\n"
"</style>\n"
"</head>\n"
"<body><main>\n"
"<header>\n"
"<div class=\"brand\">\n"
"<svg viewBox=\"0 0 256 256\" width=\"34\" height=\"34\" aria-hidden=\"true\">\n"
"<defs><linearGradient id=\"brand-bg\" x1=\"0\" y1=\"0\" x2=\"1\" y2=\"1\"><stop offset=\"0\" stop-color=\"#0d8c7e\"/><stop offset=\"1\" stop-color=\"#0a6f63\"/></linearGradient><clipPath id=\"brand-inner\"><rect x=\"55\" y=\"55\" width=\"146\" height=\"146\" rx=\"27\"/></clipPath></defs>\n"
"<rect width=\"256\" height=\"256\" rx=\"72\" fill=\"url(#brand-bg)\"/>\n"
"<g clip-path=\"url(#brand-inner)\" fill=\"white\" fill-opacity=\".85\"><rect x=\"128\" y=\"55\" width=\"73\" height=\"73\"/><rect x=\"55\" y=\"128\" width=\"73\" height=\"73\"/></g>\n"
"</svg><span>Tesserae</span>\n"
"</div><h1>Device setup</h1>\n"
"</header>\n"
"<dl class=\"status\"><dt>Device</dt><dd>PicPak</dd><dt>Firmware</dt><dd><code>{{version}}</code></dd><dt>Device ID</dt><dd><code>{{status_id}}</code></dd><dt>Connection</dt><dd>{{status_transport}}</dd><dt>{{status_endpoint_label}}</dt><dd><code>{{status_endpoint}}</code></dd><dt>Setup IP</dt><dd><code>192.168.4.1</code></dd></dl>\n"
"<div class=\"notice\" role=\"alert\">{{error}}</div>\n"
"<form method=\"post\" action=\"/save\" id=\"setup-form\">\n"
"<input type=\"hidden\" name=\"nonce\" value=\"{{nonce}}\">\n"
"<section class=\"card\"><h2>Wi-Fi network</h2>\n"
"<div class=\"field\"><label for=\"ssid\">Network name (SSID)</label><input id=\"ssid\" name=\"ssid\" value=\"{{ssid}}\" maxlength=\"32\" required placeholder=\"my-home-wifi\" autocomplete=\"off\" autocapitalize=\"none\" spellcheck=\"false\"></div>\n"
"<div class=\"field\"><label for=\"network\">Or pick a nearby network</label><select id=\"network\"><option value=\"\">Choose a network</option>{{networks}}</select><p class=\"hint\">2.4 GHz networks found when setup opened. For a hidden network, enter its name above.</p></div>\n"
"<div class=\"field\"><label for=\"pass\">Password</label><div class=\"password\"><input id=\"pass\" name=\"pass\" type=\"password\" maxlength=\"64\" autocomplete=\"new-password\"><button type=\"button\" id=\"toggle\" aria-label=\"Show password\" aria-controls=\"pass\" aria-pressed=\"false\">Show</button></div><p class=\"hint\">Leave blank to keep the password for the same network, or to join a different open network.</p></div>\n"
"</section>\n"
"<section class=\"card\"><h2 id=\"refresh-title\">Refresh speed</h2>\n"
"<fieldset aria-labelledby=\"refresh-title\">\n"
"<label class=\"radio\" for=\"wf-5s\"><input type=\"radio\" name=\"waveform\" value=\"5s\" id=\"wf-5s\" {{wave_5s}}><span><strong>5 s</strong><span class=\"badge\">Experimental</span><span class=\"hint\">Fastest updates. Colors may look softer than Native.</span></span></label>\n"
"<label class=\"radio\" for=\"wf-10s\"><input type=\"radio\" name=\"waveform\" value=\"10s\" id=\"wf-10s\" {{wave_10s}}><span><strong>10 s</strong><span class=\"badge\">Experimental</span><span class=\"hint\">A slower fast mode. Compare color quality on your display.</span></span></label>\n"
"<label class=\"radio\" for=\"wf-native\"><input type=\"radio\" name=\"waveform\" value=\"native\" id=\"wf-native\" {{wave_native}}><span><strong>Native</strong><span class=\"badge\">Default</span><span class=\"hint\">Uses the panel's built-in waveform. Refresh takes longer.</span></span></label>\n"
"</fieldset>\n"
"<div class=\"refresh-help\"><p class=\"hint\">Your refresh choice is saved with the other settings using Save &amp; restart below. The selected speed applies to images and all built-in screens. Native is the default until you choose another speed. Hold the front button for 20 seconds to return to setup.</p></div>\n"
"</section>\n"
"<section class=\"card\"><h2>Connection</h2>\n"
"<noscript><p class=\"hint\">Fill in the fields for your selected connection only.</p></noscript>\n"
"<label class=\"radio\"><input type=\"radio\" name=\"transport\" value=\"rest\" id=\"transport-rest\" {{transport_rest}}><span><strong>Local REST</strong><span class=\"hint\">Connect directly to your Tesserae server.</span></span></label>\n"
"<label class=\"radio\"><input type=\"radio\" name=\"transport\" value=\"cloud\" id=\"transport-cloud\" {{transport_cloud}}><span><strong>Cloud</strong><span class=\"hint\">Connect to Tesserae Cloud with a claim code.</span></span></label>\n"
"<label class=\"radio\"><input type=\"radio\" name=\"transport\" value=\"relay\" id=\"transport-relay\" {{transport_relay}}><span><strong>Relay</strong><span class=\"hint\">Receive encrypted images through a relay.</span></span></label>\n"
"<label class=\"radio\"><input type=\"radio\" name=\"transport\" value=\"mqtt\" id=\"transport-mqtt\" {{transport_mqtt}}><span><strong>MQTT broker</strong><span class=\"hint\">Subscribe to retained frame topics on your broker.</span></span></label>\n"
"</section>\n"
"<section class=\"card\" id=\"mqtt-fields\"><h2>MQTT broker</h2>\n"
"<div class=\"field\"><label for=\"mqtt-id\">Device ID (optional)</label><input id=\"mqtt-id\" name=\"mqtt_id\" value=\"{{mqtt_id}}\" maxlength=\"32\" pattern=\"[a-z][a-z0-9_\\-]{1,31}\" placeholder=\"picpak-kitchen\"><p class=\"hint\">Leave blank to keep this connection's saved ID or use a MAC-based ID.</p></div>\n"
"<div class=\"field\"><label for=\"mqtt-uri\">Broker URI</label><input id=\"mqtt-uri\" name=\"mqtt_uri\" value=\"{{mqtt_uri}}\" maxlength=\"159\" data-required placeholder=\"mqtt://192.168.1.50:1883\"></div>\n"
"<div class=\"field\"><label for=\"mqtt-user\">Username (optional)</label><input id=\"mqtt-user\" name=\"mqtt_user\" value=\"{{mqtt_user}}\" maxlength=\"63\" autocomplete=\"off\"></div>\n"
"<div class=\"field\"><label for=\"mqtt-pass\">Password (optional)</label><input id=\"mqtt-pass\" name=\"mqtt_pass\" type=\"password\" maxlength=\"63\" autocomplete=\"new-password\"><p class=\"hint\">Blank keeps the password only for the same broker and username. MQTT uses broker credentials and device topics, without a pairing code.</p></div>\n"
"</section>\n"
"<section class=\"card\" id=\"rest-fields\"><h2>Local REST</h2>\n"
"<div class=\"field\"><label for=\"id\">Device ID <span class=\"muted\">(optional)</span></label><input id=\"id\" name=\"id\" value=\"{{id}}\" maxlength=\"32\" pattern=\"[a-z][a-z0-9_\\-]{1,31}\" placeholder=\"picpak-kitchen\" autocomplete=\"off\" autocapitalize=\"none\" spellcheck=\"false\"><p class=\"hint\">Leave blank to keep the saved ID for this connection. New devices get a name based on their MAC address, such as picpak-a1b2c3.</p></div>\n"
"<div class=\"field\"><label for=\"server\">Server URL</label><input id=\"server\" name=\"server\" value=\"{{server}}\" maxlength=\"159\" data-required placeholder=\"http://tesserae.local:8765\" autocomplete=\"off\" autocapitalize=\"none\" spellcheck=\"false\"><p class=\"hint\">Enter your Tesserae server address, starting with http:// or https://. Your display must be able to connect to it. For HTTPS, use a certificate from a trusted certificate provider.</p></div>\n"
"<div class=\"field\"><label for=\"pair\">Pairing code <span class=\"muted\">(optional)</span></label><input id=\"pair\" name=\"pair\" inputmode=\"numeric\" maxlength=\"6\" pattern=\"[0-9]{6}\" placeholder=\"6-digit code\" autocomplete=\"off\"><p class=\"hint\">Leave blank and register in Settings → Devices, or enter a code from Pair new device. An existing pairing is kept when the server and device ID stay the same.</p></div>\n"
"</section>\n"
"<section class=\"card\" id=\"cloud-fields\"><h2>Tesserae Cloud</h2>\n"
"<p class=\"hint\">Connects to <code>{{cloud_url}}</code>.</p>\n"
"<div class=\"field\"><label for=\"cloud-id\">Device ID <span class=\"muted\">(optional)</span></label><input id=\"cloud-id\" name=\"cloud_id\" value=\"{{cloud_id}}\" maxlength=\"32\" pattern=\"[a-z][a-z0-9_\\-]{1,31}\" placeholder=\"picpak-kitchen\" autocomplete=\"off\" autocapitalize=\"none\" spellcheck=\"false\"><p class=\"hint\">Leave blank to keep the saved ID for this connection. Use 2–32 lowercase letters, digits, hyphens or underscores, starting with a letter. Changing the ID requires a new claim code.</p></div>\n"
"<div class=\"field\"><label for=\"cloud-code\">Cloud claim code <span class=\"muted\">(optional when paired)</span></label><input id=\"cloud-code\" name=\"cloud_code\" inputmode=\"numeric\" maxlength=\"15\" pattern=\"[0-9 ]{8,15}\" placeholder=\"1234 5678\" autocomplete=\"off\"><p class=\"hint\">In Tesserae Cloud, open Settings → Panels → Add a panel and enter the eight-digit code. Leave blank to keep an existing pairing when the Device ID stays the same, including Wi-Fi or refresh-speed changes. A new code starts pairing again.</p></div>\n"
"</section>\n"
"<section class=\"card\" id=\"relay-fields\"><h2>Relay</h2>\n"
"<div class=\"field\"><label for=\"relay-url\">Relay URL</label><input id=\"relay-url\" name=\"relay_url\" value=\"{{relay_url}}\" placeholder=\"https://relay.tesserae.ink\" maxlength=\"159\" data-required autocomplete=\"off\" autocapitalize=\"none\" spellcheck=\"false\"><p class=\"hint\">Use the relay address configured in your Tesserae server. Use HTTPS with a trusted certificate.</p></div>\n"
"<div class=\"field\"><label for=\"relay-code\">Relay pairing code</label><input id=\"relay-code\" name=\"relay_code\" maxlength=\"32\" pattern=\"[A-Za-z0-9-]+\" autocomplete=\"off\" autocapitalize=\"none\" spellcheck=\"false\"><p class=\"hint\">Create a cloud relay pairing code in Tesserae and enter it here. Leave blank to keep an existing relay pairing at the same address.</p></div>\n"
"<p class=\"hint\">If this device is removed from the relay, its screen will ask you to hold the front button for 20 seconds to open setup.</p>\n"
"</section>\n"
"<button class=\"submit\" id=\"submit\" type=\"submit\">Save &amp; restart</button>\n"
"<p class=\"hint\" id=\"progress\" role=\"status\">Saves all settings above. After restarting, the display checks its connection. If setup fails, reconnect to its setup Wi-Fi for details.</p>\n"
"</form>\n"
"<footer>Dashboard companion · <span>dmellok/tesserae</span><br>PicPak firmware · <span>varanu5</span></footer>\n"
"<script>\n"
"function updateTransport(){\n"
"const relay=document.getElementById('transport-relay').checked,cloud=document.getElementById('transport-cloud').checked,mqtt=document.getElementById('transport-mqtt').checked;\n"
"for(const [id,active] of [['mqtt-fields',mqtt],['rest-fields',!relay&&!cloud&&!mqtt],['cloud-fields',cloud],['relay-fields',relay]]){\n"
"const section=document.getElementById(id);section.hidden=!active;\n"
"section.querySelectorAll('input').forEach(input=>{input.disabled=!active;input.required=active&&input.hasAttribute('data-required');});\n"
"}}\n"
"document.getElementById('transport-rest').addEventListener('change',updateTransport);\n"
"document.getElementById('transport-cloud').addEventListener('change',updateTransport);\n"
"document.getElementById('transport-relay').addEventListener('change',updateTransport);\n"
"document.getElementById('transport-mqtt').addEventListener('change',updateTransport);\n"
"updateTransport();\n"
"const ssid=document.getElementById('ssid'),pw=document.getElementById('pass');\n"
"document.getElementById('network').addEventListener('change',e=>{const o=e.target.selectedOptions[0];if(!o.value)return;ssid.value=o.value;pw.value='';pw.focus();});\n"
"document.getElementById('toggle').addEventListener('click',e=>{const show=pw.type==='password';pw.type=show?'text':'password';e.target.textContent=show?'Hide':'Show';e.target.setAttribute('aria-label',show?'Hide password':'Show password');e.target.setAttribute('aria-pressed',String(show));});\n"
"document.getElementById('setup-form').addEventListener('submit',()=>{const button=document.getElementById('submit');button.disabled=true;button.textContent='Saving…';document.getElementById('progress').textContent='Saving all settings. Keep this page open until the display restarts.';});\n"
"</script>\n"
"</main></body>\n"
"</html>\n"
;
static char s_nonce[33];
typedef struct {
    httpd_req_t *req;
    char ssid[200], server[960], relay_url[960], id[384], error[768], networks[8192];
    char cloud_id[384], status_id[384], status_endpoint[960];
    char mqtt_id[384], mqtt_uri[960], mqtt_user[384];
    bool mqtt, saved_mqtt;
    bool relay, cloud, saved_relay, saved_cloud;
    display_mode_t display_mode;
} view_t;
static const char *view_value(const char *key,void *context) {
    view_t *v=context;
    if (!strcmp(key,"ssid")) return v->ssid;
    if (!strcmp(key,"server")) return v->server;
    if (!strcmp(key,"relay_url")) return v->relay_url;
    if (!strcmp(key,"transport_rest")) return v->relay || v->cloud || v->mqtt ? "" : "checked";
    if (!strcmp(key,"transport_relay")) return v->relay ? "checked" : "";
    if (!strcmp(key,"transport_cloud")) return v->cloud ? "checked" : "";
    if (!strcmp(key,"transport_mqtt")) return v->mqtt ? "checked" : "";
    if (!strcmp(key,"mqtt_id")) return v->mqtt_id;
    if (!strcmp(key,"mqtt_uri")) return v->mqtt_uri;
    if (!strcmp(key,"mqtt_user")) return v->mqtt_user;
    if (!strcmp(key,"cloud_url")) return SETUP_CLOUD_URL;
    if (!strcmp(key,"status_id")) return v->status_id;
    if (!strcmp(key,"status_endpoint")) return v->status_endpoint;
    if (!strcmp(key,"status_endpoint_label")) return v->saved_relay ? "Relay" : v->saved_mqtt ? "Broker" : "Server";
    if (!strcmp(key,"status_transport")) return v->saved_relay ? "Relay" : v->saved_cloud ? "Cloud" : v->saved_mqtt ? "MQTT broker" : "Local REST";
    if (!strcmp(key,"id")) return v->id;
    if (!strcmp(key,"cloud_id")) return v->cloud_id;
    if (!strcmp(key,"error")) return v->error;
    if (!strcmp(key,"networks")) return v->networks;
    if (!strcmp(key,"nonce")) return s_nonce;
    if (!strcmp(key,"version")) return FW_VERSION;
    if (!strcmp(key,"wave_native")) return v->display_mode == DISPLAY_NATIVE ? "checked" : "";
    if (!strcmp(key,"wave_5s")) return v->display_mode == DISPLAY_FAST_5S ? "checked" : "";
    if (!strcmp(key,"wave_10s")) return v->display_mode == DISPLAY_FAST_10S ? "checked" : "";
    return NULL;
}
static bool view_send(const char *data,size_t n,void *context) {
    if (!n) return true;
    return httpd_resp_send_chunk(((view_t *)context)->req,data,n)==ESP_OK;
}
static esp_err_t form(httpd_req_t *req,const char *error,const setup_request_t *submitted) {
    view_t *v=calloc(1,sizeof(*v));
    if (!v) return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Not enough memory. Retry setup.");
    v->req=req;
    v->display_mode=(display_mode_t)config_get_waveform(DEFAULT_WAVEFORM);
    char saved_id[64], endpoint[160];
    v->saved_mqtt=config_active_profile()==CONFIG_PROFILE_MQTT;
    v->saved_relay=config_relay_configured();
    config_get_server_url(endpoint,sizeof endpoint);
    v->saved_cloud=config_active_profile()==CONFIG_PROFILE_CLOUD;
    if(v->saved_mqtt) {
        config_profile_view_t mqtt; config_get_profile(CONFIG_PROFILE_MQTT,&mqtt);
        strlcpy(endpoint,mqtt.server,sizeof endpoint);
    }
    if (v->saved_relay) {
        config_get_relay_url(endpoint,sizeof endpoint);
        config_get_relay_device(saved_id,sizeof saved_id);
        if (!saved_id[0]) strcpy(saved_id,"Assigned after pairing");
    } else {
        config_get_device_id(saved_id,sizeof saved_id);
        if (!saved_id[0]) {
            uint8_t mac[6];
            if (esp_read_mac(mac,ESP_MAC_WIFI_STA)==ESP_OK)
                snprintf(saved_id,sizeof saved_id,"picpak-%02x%02x%02x",mac[3],mac[4],mac[5]);
            else strcpy(saved_id,"Not assigned");
        }
    }
    provform_html_escape(saved_id,v->status_id,sizeof v->status_id);
    provform_html_escape(endpoint[0] ? endpoint : "Not configured",v->status_endpoint,sizeof v->status_endpoint);
    config_profile_view_t local, cloud, relay, mqtt;
    config_get_profile(CONFIG_PROFILE_MQTT, &mqtt);
    config_get_profile(CONFIG_PROFILE_LOCAL, &local);
    config_get_profile(CONFIG_PROFILE_CLOUD, &cloud);
    config_get_profile(CONFIG_PROFILE_RELAY, &relay);
    setup_request_t fields={0};
    config_get_wifi(fields.ssid,sizeof fields.ssid,fields.pass,sizeof fields.pass);
    fields.relay = v->saved_relay;
    fields.cloud = v->saved_cloud;
    fields.mqtt = v->saved_mqtt;
    if (submitted) {
        fields = *submitted;
        if (fields.relay) strlcpy(relay.server, fields.relay_url, sizeof relay.server);
        else if (fields.cloud) strlcpy(cloud.id, fields.id, sizeof cloud.id);
        else if(fields.mqtt) {
            strlcpy(mqtt.server,fields.mqtt_uri,sizeof mqtt.server);
            strlcpy(mqtt.id,fields.id,sizeof mqtt.id);
            strlcpy(mqtt.user,fields.mqtt_user,sizeof mqtt.user);
        } else {
            strlcpy(local.server, fields.server, sizeof local.server);
            strlcpy(local.id, fields.id, sizeof local.id);
        }
    }
    if (!provform_device_id_valid(local.id)) local.id[0]=0;
    if (!provform_device_id_valid(cloud.id)) cloud.id[0]=0;
    v->relay = fields.relay;
    v->cloud = fields.cloud;
    v->mqtt = fields.mqtt;
    provform_html_escape(mqtt.id,v->mqtt_id,sizeof v->mqtt_id);
    provform_html_escape(mqtt.server,v->mqtt_uri,sizeof v->mqtt_uri);
    provform_html_escape(mqtt.user,v->mqtt_user,sizeof v->mqtt_user);
    provform_html_escape(relay.server,v->relay_url,sizeof v->relay_url);
    if (fields.has_display_mode) v->display_mode=fields.display_mode;
    provform_html_escape(fields.ssid,v->ssid,sizeof v->ssid);
    provform_html_escape(local.server,v->server,sizeof v->server);
    provform_html_escape(local.id,v->id,sizeof v->id);
    provform_html_escape(cloud.id,v->cloud_id,sizeof v->cloud_id);
    provform_html_escape(error ? error : "",v->error,sizeof v->error);
    memset(&fields,0,sizeof fields);
    size_t used=0;
    for (unsigned i=0;i<s_scan_count;i++) {
        char escaped[200]; provform_html_escape(s_scan[i].ssid,escaped,sizeof escaped);
        int n=snprintf(v->networks+used,sizeof v->networks-used,
            "<option value=\"%s\">%s (%d dBm)</option>",
            escaped,escaped,s_scan[i].rssi);
        if (n<0 || (size_t)n>=sizeof v->networks-used) { v->networks[used]=0; break; }
        used+=(size_t)n;
    }
    httpd_resp_set_type(req,"text/html; charset=utf-8");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    httpd_resp_set_hdr(req,"X-Content-Type-Options","nosniff");
    bool ok=portal_render(k_portal_html,view_value,view_send,v);
    free(v);
    return ok ? httpd_resp_send_chunk(req,NULL,0) : ESP_FAIL;
}
static const char *s_initial_note;
static esp_err_t h_root(httpd_req_t *req) { return form(req,s_initial_note,NULL); }
static esp_err_t h_save(httpd_req_t *req) {
    static char body[3072];
    if (!req->content_len || req->content_len >= sizeof body) {
        httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Form too large. Reload setup.");
        return ESP_FAIL;
    }
    size_t used=0;
    while(used<req->content_len) {
        int n=httpd_req_recv(req,body+used,req->content_len-used);
        if(n<=0) { memset(body,0,sizeof body); return ESP_FAIL; }
        used+=(size_t)n;
    }
    body[used]=0;
    char old_ssid[33],old_pass[65],transport[16];
    config_get_wifi(old_ssid,sizeof old_ssid,old_pass,sizeof old_pass);
    provform_parse_field(body,"transport",transport,sizeof transport);
    config_profile_t selected=!strcmp(transport,"cloud") ? CONFIG_PROFILE_CLOUD :
        !strcmp(transport,"relay") ? CONFIG_PROFILE_RELAY :
        !strcmp(transport,"mqtt") ? CONFIG_PROFILE_MQTT : CONFIG_PROFILE_LOCAL;
    config_profile_view_t profile;
    config_get_profile(selected,&profile);
    setup_request_t fields;
    const char *error=setup_request_parse(body,s_nonce,old_ssid,old_pass,profile.id,&fields);
    memset(old_pass,0,sizeof old_pass); memset(body,0,sizeof body);
    if(!error && fields.cloud)
        error=setup_request_check_cloud_pairing(&fields,profile.server,profile.id,profile.paired);
    if(!error && fields.relay && !fields.relay_code[0] &&
        ((!profile.paired && !profile.pending) || strcmp(profile.server,fields.relay_url)))
        error="Enter a new Relay pairing code from your server.";
    if(!error) {
        const char *server=fields.relay ? fields.relay_url : fields.mqtt ? fields.mqtt_uri : fields.server;
        const char *code=fields.relay ? fields.relay_code : fields.pair;
        esp_err_t err=config_save_connection(selected,server,fields.id,code,fields.mqtt_user,fields.mqtt_pass);
        if(err==ESP_OK) err=config_save_wifi(fields.ssid,fields.pass);
        if(err==ESP_OK && fields.has_display_mode) err=config_save_waveform(fields.display_mode);
        if(err!=ESP_OK) error="Settings could not be fully saved. Please try again.";
    }
    if(error) {
        httpd_resp_set_status(req,"400 Bad Request");
        esp_err_t result=form(req,error,&fields);
        memset(&fields,0,sizeof fields); return result;
    }
    memset(&fields,0,sizeof fields);
    config_set_paired_pending(true);
    httpd_resp_set_type(req,"text/html; charset=utf-8");
    httpd_resp_sendstr(req,"<!doctype html><html lang=en><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>Settings saved</title><style>body{font:17px/1.5 system-ui;background:#f1f0ec;color:#18181b;padding:40px 20px}main{max-width:420px;margin:auto;background:white;padding:28px;border-radius:12px}h1{color:#0d8c7e;font-size:26px}</style><main><h1>Settings saved</h1><p>Your PicPak is restarting and checking its connection.</p><p>Return to your home Wi-Fi and open Tesserae. Register Local REST or MQTT devices in Settings &rarr; Devices if needed. Cloud claims the panel automatically. Relay pairing completes when your server is online.</p><p>If setup reopens, reconnect to the setup network for details.</p></main></html>");
    xEventGroupSetBits(s_done,BIT_CREDS_SAVED);
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
    char ssid[SETUP_SSID_CAPACITY];
    ESP_ERROR_CHECK(setup_ap_ssid(ssid));
    memcpy(wc.ap.ssid, ssid, strlen(ssid));
    strncpy((char *)wc.ap.password, PROVISION_AP_PASS, sizeof(wc.ap.password) - 1);
    wc.ap.ssid_len = strlen(ssid);
    if (strlen(PROVISION_AP_PASS) < 8) wc.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    // PicPak brownout guard: cap TX power before the radio transmits (as in STA).
    esp_wifi_set_max_tx_power(WIFI_TX_POWER_QDBM);

    ESP_LOGI(TAG, "AP up: ssid=%s ip=192.168.4.1", ssid);
}

static void start_http(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 6;
    cfg.stack_size = 16384;   // render_form holds several KB of locals at once
    // Leave sockets for HTTP control, captive DNS and other network services.
    // Recycle idle browser connections before the shared socket pool fills.
    cfg.max_open_sockets = 4;
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
    uint8_t nonce[16]; esp_fill_random(nonce,sizeof nonce);
    for(unsigned i=0;i<sizeof nonce;i++) snprintf(s_nonce+i*2,3,"%02x",nonce[i]);
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
