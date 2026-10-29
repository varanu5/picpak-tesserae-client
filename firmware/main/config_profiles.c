// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include "config_store.h"
#include "defaults.h"
#include "nvs.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    char server[160], id[64], code[16], token[80], etag[80];
    uint32_t ever_paired, unpaired;
} rest_t;
typedef struct {
    char url[160], code[40], install[64], device[64], token[256];
    char etag[80], config_etag[80];
    uint8_t priv[32], key[32];
    uint32_t has_priv, has_key, revoked;
} relay_t;
typedef struct {
    uint32_t version, active, sleep[4];
    rest_t rest[2];
    relay_t relay;
    char mqtt_uri[160], mqtt_id[64], mqtt_user[64], mqtt_pass[64], frame_url[256];
} profiles_t;
static profiles_t s_profiles;
static const char *TAG = "profiles";
static unsigned rest_index(void) { return s_profiles.active == CONFIG_PROFILE_CLOUD ? 1 : 0; }
static bool rest_active(void) {
    return s_profiles.active == CONFIG_PROFILE_LOCAL || s_profiles.active == CONFIG_PROFILE_CLOUD;
}
static esp_err_t persist(const profiles_t *next) {
    if (!memcmp(next, &s_profiles, sizeof *next)) return ESP_OK;
    nvs_handle_t h;
    esp_err_t err = nvs_open("connections", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, "profiles", next, sizeof *next);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) s_profiles = *next;
    else ESP_LOGE(TAG, "connection save failed: %s", esp_err_to_name(err));
    return err;
}
static profiles_t *copy(void) {
    profiles_t *v = malloc(sizeof *v);
    if (v) *v = s_profiles;
    return v;
}
static esp_err_t finish(profiles_t *v) {
    esp_err_t err = persist(v);
    memset(v, 0, sizeof *v);
    free(v);
    return err;
}
static void legacy_str(const char *ns, const char *key, char *out, size_t cap) {
    nvs_handle_t h;
    out[0] = 0;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return;
    size_t n = cap;
    if (nvs_get_str(h, key, out, &n) != ESP_OK) out[0] = 0;
    nvs_close(h);
}
static bool legacy_key(const char *key, uint8_t *out) {
    nvs_handle_t h;
    if (nvs_open("relay", NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = 32;
    bool ok = nvs_get_blob(h, key, out, &n) == ESP_OK && n == 32;
    nvs_close(h);
    return ok;
}
#define TERMINATED(v) (memchr((v), 0, sizeof(v)) != NULL)
static bool valid(const profiles_t *v) {
    if (v->version != 1 || v->active > CONFIG_PROFILE_CLOUD) return false;
    for (unsigned i=0; i<2; i++) {
        const rest_t *r = &v->rest[i];
        if (!TERMINATED(r->server) || !TERMINATED(r->id) || !TERMINATED(r->code) ||
            !TERMINATED(r->token) || !TERMINATED(r->etag) || r->ever_paired > 1 || r->unpaired > 1)
            return false;
    }
    const relay_t *r = &v->relay;
    return TERMINATED(r->url) && TERMINATED(r->code) && TERMINATED(r->install) &&
        TERMINATED(r->device) && TERMINATED(r->token) && TERMINATED(r->etag) &&
        TERMINATED(r->config_etag) && r->has_priv <= 1 && r->has_key <= 1 && r->revoked <= 1 &&
        TERMINATED(v->mqtt_uri) && TERMINATED(v->mqtt_id) && TERMINATED(v->mqtt_user) &&
        TERMINATED(v->mqtt_pass) && TERMINATED(v->frame_url);
}
esp_err_t config_profiles_init(void) {
    memset(&s_profiles, 0, sizeof s_profiles);
    nvs_handle_t h;
    esp_err_t err = nvs_open("connections", NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t n = sizeof s_profiles;
        err = nvs_get_blob(h, "profiles", &s_profiles, &n);
        nvs_close(h);
        if (err == ESP_OK) return n == sizeof s_profiles && valid(&s_profiles) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND) return err;
    memset(&s_profiles, 0, sizeof s_profiles);
    s_profiles.version = 1;
    s_profiles.active = CONFIG_PROFILE_LOCAL;
    char server[160];
    legacy_str("rest", "server_url", server, sizeof server);
    if (!server[0]) strlcpy(server, REST_DEFAULT_SERVER_URL, sizeof server);
    unsigned index = !strcmp(server, SETUP_CLOUD_URL) ? 1 : 0;
    rest_t *r = &s_profiles.rest[index];
    strlcpy(r->server, server, sizeof r->server);
    strlcpy(s_profiles.rest[1].server, SETUP_CLOUD_URL, sizeof s_profiles.rest[1].server);
#define IMPORT(ns,key,field) legacy_str(ns,key,field,sizeof(field))
    IMPORT("rest", "device_id", r->id);
    IMPORT("rest", "pair_code", r->code);
    IMPORT("rest", "device_token", r->token);
    IMPORT("rest", "frame_etag", r->etag);
    r->ever_paired = r->token[0] != 0;
    IMPORT("rest", "mqtt_uri", s_profiles.mqtt_uri);
    IMPORT("rest", "mqtt_user", s_profiles.mqtt_user);
    IMPORT("rest", "mqtt_pass", s_profiles.mqtt_pass);
    IMPORT("rest", "frame_url", s_profiles.frame_url);
    strlcpy(s_profiles.mqtt_id, r->id, sizeof s_profiles.mqtt_id);
    if (!s_profiles.mqtt_uri[0]) strlcpy(s_profiles.mqtt_uri, MQTT_DEFAULT_URI, sizeof s_profiles.mqtt_uri);
    if (!s_profiles.mqtt_user[0]) strlcpy(s_profiles.mqtt_user, MQTT_DEFAULT_USER, sizeof s_profiles.mqtt_user);
    if (!s_profiles.mqtt_pass[0]) strlcpy(s_profiles.mqtt_pass, MQTT_DEFAULT_PASS, sizeof s_profiles.mqtt_pass);
    relay_t *q = &s_profiles.relay;
    IMPORT("relay", "url", q->url); IMPORT("relay", "code", q->code);
    IMPORT("relay", "install", q->install); IMPORT("relay", "device", q->device);
    IMPORT("relay", "token", q->token); IMPORT("relay", "etag", q->etag);
    IMPORT("relay", "cfg_etag", q->config_etag);
    q->has_priv = legacy_key("priv", q->priv);
    q->has_key = legacy_key("key", q->key);
#undef IMPORT
    uint8_t mode = 1;
    if (nvs_open("rest", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "transport", &mode); nvs_close(h);
    }
    s_profiles.active = q->url[0] && (q->has_key || q->code[0]) ? CONFIG_PROFILE_RELAY :
        mode == 0 ? CONFIG_PROFILE_MQTT : index ? CONFIG_PROFILE_CLOUD : CONFIG_PROFILE_LOCAL;
    if (nvs_open("state", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "sleep_s", &s_profiles.sleep[s_profiles.active]); nvs_close(h);
    }
    return ESP_OK;
}
config_profile_t config_active_profile(void) { return (config_profile_t)s_profiles.active; }
void config_get_profile(config_profile_t mode, config_profile_view_t *out) {
    memset(out, 0, sizeof *out);
    if (mode == CONFIG_PROFILE_RELAY) {
        const relay_t *r = &s_profiles.relay;
        strlcpy(out->server, r->url, sizeof out->server);
        strlcpy(out->id, r->device, sizeof out->id);
        out->paired = !r->revoked && r->has_key && r->token[0] && r->install[0] && r->device[0];
        out->pending = !r->revoked && r->code[0];
    } else if (mode == CONFIG_PROFILE_MQTT) {
        strlcpy(out->server, s_profiles.mqtt_uri, sizeof out->server);
        strlcpy(out->id, s_profiles.mqtt_id, sizeof out->id);
        strlcpy(out->user, s_profiles.mqtt_user, sizeof out->user);
    } else if (mode == CONFIG_PROFILE_LOCAL || mode == CONFIG_PROFILE_CLOUD) {
        const rest_t *r = &s_profiles.rest[mode == CONFIG_PROFILE_CLOUD];
        strlcpy(out->server, r->server, sizeof out->server);
        strlcpy(out->id, r->id, sizeof out->id);
        out->paired = r->token[0] != 0;
        out->pending = r->code[0] != 0;
    }
}
#define GETTER(name,field) void name(char *out,size_t cap) { if(cap) strlcpy(out,s_profiles.field,cap); }
#define SETTER(name,field) void name(const char *value) { \
    if(!value || strlen(value)>=sizeof s_profiles.field || !strcmp(value,s_profiles.field)) return; \
    profiles_t *v=copy(); if(!v) return; strlcpy(v->field,value,sizeof v->field); (void)finish(v); }
GETTER(config_get_server_url,rest[rest_index()].server)
GETTER(config_get_device_token,rest[rest_index()].token)
GETTER(config_get_pairing_code,rest[rest_index()].code)
GETTER(config_get_etag,rest[rest_index()].etag)
SETTER(config_set_server_url,rest[rest_index()].server)
SETTER(config_set_pairing_code,rest[rest_index()].code)
SETTER(config_set_etag,rest[rest_index()].etag)
void config_get_device_id(char *out,size_t cap) {
    if (cap) strlcpy(out, s_profiles.active == CONFIG_PROFILE_MQTT ? s_profiles.mqtt_id :
        s_profiles.active == CONFIG_PROFILE_RELAY ? s_profiles.relay.device : s_profiles.rest[rest_index()].id, cap);
}
void config_set_device_id(const char *id) {
    if (!id || strlen(id)>=64) return;
    profiles_t *v=copy(); if(!v) return;
    char *dest = v->active == CONFIG_PROFILE_MQTT ? v->mqtt_id : v->rest[rest_index()].id;
    strlcpy(dest,id,64); (void)finish(v);
}
void config_set_device_token(const char *token) {
    if(!token || strlen(token)>=sizeof s_profiles.rest[0].token) return;
    profiles_t *v=copy(); if(!v) return;
    rest_t *r=&v->rest[rest_index()]; strlcpy(r->token,token,sizeof r->token);
    if(token[0]) { r->ever_paired=1; r->unpaired=0; }
    (void)finish(v);
}
esp_err_t config_save_pairing(const char *id,const char *token,bool consume_code) {
    if(!rest_active() || !id || !id[0] || strlen(id)>=64 || !token || !token[0] || strlen(token)>=80)
        return ESP_ERR_INVALID_ARG;
    profiles_t *v=copy(); if(!v) return ESP_ERR_NO_MEM;
    rest_t *r=&v->rest[rest_index()];
    strlcpy(r->id,id,sizeof r->id); strlcpy(r->token,token,sizeof r->token);
    r->ever_paired=1; r->unpaired=0; if(consume_code) r->code[0]=0;
    return finish(v);
}
bool config_rest_unpaired(void) { return rest_active() && s_profiles.rest[rest_index()].unpaired; }
esp_err_t config_note_rest_unregistered(void) {
    if(!rest_active()) return ESP_ERR_INVALID_STATE;
    const rest_t *r=&s_profiles.rest[rest_index()];
    if(!r->ever_paired || r->unpaired || r->token[0]) return ESP_OK;
    profiles_t *v=copy(); if(!v) return ESP_ERR_NO_MEM;
    v->rest[rest_index()].unpaired=1; return finish(v);
}
void config_set_transport(uint8_t mode) {
    if(mode>CONFIG_PROFILE_CLOUD) return;
    profiles_t *v=copy(); if(!v) return; v->active=mode; (void)finish(v);
}
uint8_t config_get_transport(uint8_t fallback) { (void)fallback; return s_profiles.active; }
uint32_t config_get_sleep_s(uint32_t fallback) {
    uint32_t v=s_profiles.sleep[s_profiles.active];
    return v>=SLEEP_INTERVAL_MIN_S && v<=SLEEP_INTERVAL_MAX_S ? v : fallback;
}
void config_set_sleep_s(uint32_t seconds) {
    if(seconds<SLEEP_INTERVAL_MIN_S || seconds>SLEEP_INTERVAL_MAX_S) return;
    profiles_t *v=copy(); if(!v) return; v->sleep[v->active]=seconds; (void)finish(v);
}
void config_get_mqtt(char *uri,size_t us,char *user,size_t ns,char *pass,size_t ps) {
    if(us) strlcpy(uri,s_profiles.mqtt_uri,us);
    if(ns) strlcpy(user,s_profiles.mqtt_user,ns);
    if(ps) strlcpy(pass,s_profiles.mqtt_pass,ps);
}
void config_set_mqtt(const char *uri,const char *user,const char *pass) {
    if(!uri || strlen(uri)>=160 || !user || strlen(user)>=64 || (pass && strlen(pass)>=64)) return;
    profiles_t *v=copy(); if(!v) return;
    bool same=!strcmp(uri,v->mqtt_uri) && !strcmp(user,v->mqtt_user);
    strlcpy(v->mqtt_uri,uri,160); strlcpy(v->mqtt_user,user,64);
    if(pass && pass[0]) strlcpy(v->mqtt_pass,pass,64); else if(!same) v->mqtt_pass[0]=0;
    (void)finish(v);
}
GETTER(config_get_frame_url,frame_url)
SETTER(config_set_frame_url,frame_url)
void config_clear_frame_ref(void) {
    profiles_t *v=copy(); if(!v) return;
    v->rest[0].etag[0]=v->rest[1].etag[0]=v->relay.etag[0]=v->frame_url[0]=0; (void)finish(v);
}
bool config_relay_configured(void) { return s_profiles.active == CONFIG_PROFILE_RELAY; }
bool config_relay_revoked(void) { return config_relay_configured() && s_profiles.relay.revoked; }
bool config_relay_ready(void) {
    const relay_t *r=&s_profiles.relay;
    return config_relay_configured() && !r->revoked && r->has_key && r->install[0] && r->device[0] && r->token[0];
}
GETTER(config_get_relay_url,relay.url)
GETTER(config_get_relay_code,relay.code)
GETTER(config_get_relay_install,relay.install)
GETTER(config_get_relay_device,relay.device)
GETTER(config_get_relay_token,relay.token)
GETTER(config_get_relay_etag,relay.etag)
GETTER(config_get_relay_config_etag,relay.config_etag)
SETTER(config_set_relay_url,relay.url)
SETTER(config_set_relay_code,relay.code)
SETTER(config_set_relay_etag,relay.etag)
SETTER(config_set_relay_config_etag,relay.config_etag)
bool config_get_relay_priv(uint8_t out[32]) {
    if(!s_profiles.relay.has_priv) return false;
    memcpy(out,s_profiles.relay.priv,32); return true;
}
bool config_get_relay_key(uint8_t out[32]) {
    if(!config_relay_ready()) return false;
    memcpy(out,s_profiles.relay.key,32); return true;
}
esp_err_t config_set_relay_priv(const uint8_t key[32]) {
    profiles_t *v=copy(); if(!v) return ESP_ERR_NO_MEM;
    memcpy(v->relay.priv,key,32); v->relay.has_priv=1; return finish(v);
}
esp_err_t config_set_relay_paired(const char *install,const char *device,const char *token,const uint8_t key[32]) {
    if(!install || !install[0] || !device || !device[0] || !token || !token[0] ||
        strlen(install)>=64 || strlen(device)>=64 || strlen(token)>=256) return ESP_ERR_INVALID_ARG;
    profiles_t *v=copy(); if(!v) return ESP_ERR_NO_MEM;
    relay_t *r=&v->relay;
    strlcpy(r->install,install,64); strlcpy(r->device,device,64); strlcpy(r->token,token,256);
    memcpy(r->key,key,32); r->has_key=1; r->has_priv=0; r->revoked=0;
    memset(r->priv,0,32); r->code[0]=r->etag[0]=r->config_etag[0]=0;
    return finish(v);
}
void config_clear_relay(void) {
    profiles_t *v=copy(); if(!v) return;
    memset(v->relay.code,0,sizeof v->relay - offsetof(relay_t,code)); (void)finish(v);
}
void config_forget_relay_pairing(void) {
    profiles_t *v=copy(); if(!v) return;
    relay_t *r=&v->relay;
    r->has_key=r->has_priv=0; r->revoked=1;
    r->code[0]=r->token[0]=r->etag[0]=r->config_etag[0]=0;
    memset(r->key,0,32); memset(r->priv,0,32); (void)finish(v);
}
esp_err_t config_save_connection(config_profile_t mode,const char *server,const char *id,
    const char *code,const char *user,const char *password) {
    if(mode>CONFIG_PROFILE_CLOUD || !server || !id || !code || !user || !password ||
        strlen(server)>=160 || strlen(id)>=64 || strlen(code)>=40 || strlen(user)>=64 || strlen(password)>=64)
        return ESP_ERR_INVALID_ARG;
    if((mode==CONFIG_PROFILE_LOCAL || mode==CONFIG_PROFILE_CLOUD) && strlen(code)>=16)
        return ESP_ERR_INVALID_ARG;
    profiles_t *v=copy(); if(!v) return ESP_ERR_NO_MEM;
    if(mode==CONFIG_PROFILE_RELAY) {
        relay_t *r=&v->relay;
        bool keep=!r->revoked && !strcmp(server,r->url) && !code[0] && (r->has_key || r->code[0]);
        if(!keep && !code[0]) { free(v); return ESP_ERR_INVALID_ARG; }
        if(!keep) {
            memset(r,0,sizeof *r); v->sleep[mode]=0;
            strlcpy(r->url,server,sizeof r->url); strlcpy(r->code,code,sizeof r->code);
        }
    } else if(mode==CONFIG_PROFILE_MQTT) {
        bool same=!strcmp(server,v->mqtt_uri) && !strcmp(user,v->mqtt_user);
        if(!same || strcmp(id,v->mqtt_id)) v->sleep[mode]=0;
        if(password[0]) strlcpy(v->mqtt_pass,password,64); else if(!same) v->mqtt_pass[0]=0;
        strlcpy(v->mqtt_uri,server,160); strlcpy(v->mqtt_user,user,64); strlcpy(v->mqtt_id,id,64);
    } else {
        rest_t *r=&v->rest[mode==CONFIG_PROFILE_CLOUD];
        bool keep=!strcmp(server,r->server) && !strcmp(id,r->id) && !code[0];
        if(mode==CONFIG_PROFILE_CLOUD && (!keep || !r->token[0]) && !code[0]) {
            free(v); return ESP_ERR_INVALID_ARG;
        }
        if(!keep) { memset(r,0,sizeof *r); v->sleep[mode]=0; strlcpy(r->code,code,sizeof r->code); }
        strlcpy(r->server,server,sizeof r->server); strlcpy(r->id,id,sizeof r->id);
    }
    v->active=mode;
    v->rest[0].etag[0]=v->rest[1].etag[0]=v->relay.etag[0]=v->frame_url[0]=0;
    return finish(v);
}
