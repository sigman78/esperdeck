/*
 * ssh_import — device implementation.
 *
 * esp_http_server serving a one-page form; a POST stores an optional private
 * key blob and appends a connection profile to profiles.ini. Two transports
 * (see ssh_import.h): SOFTAP (own WPA2 AP, random passphrase = PoP) and WEB
 * (server on the existing STA/LAN IP). The proof code is random per session.
 */

#include "ssh_import.h"
#include "wifi_manager.h"
#include "storage.h"
#include "keystore.h"       /* keystore_wipe — see the RAM hygiene note below */

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "qrcode.h"

#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "ssh_import";

/* Body cap; the private-key decode buffer is sized to match so a key field
 * can never be silently truncated within an accepted body. */
#define BODY_MAX 16384

static volatile int s_state = SSH_IMPORT_ST_IDLE;
static volatile int s_mode  = SSH_IMPORT_SOFTAP;
static char s_service[32] = "";
static char s_pop[24]     = "";   /* 16 hex chars + NUL (see gen_code) */
static char s_url[40]     = "";
static char s_last[32]    = "";
static char s_err[64]     = "";
static volatile int s_count   = 0;
static volatile int s_deleted = 0;
static volatile int s_pop_fails = 0;

static httpd_handle_t  s_httpd    = NULL;
static esp_netif_t    *s_ap_netif = NULL;
static SemaphoreHandle_t s_lock   = NULL;   /* serialize POST handling */

/* Bit-packed QR module matrix (RAM-frugal), same scheme as wifi_provision. */
#define QR_MAX 45
static uint8_t s_qr_mod[(QR_MAX * QR_MAX + 7) / 8];
static int     s_qr_size = 0;

static inline void qr_setbit(int i) { s_qr_mod[i >> 3] |= (uint8_t)(1u << (i & 7)); }
static inline bool qr_getbit(int i) { return (s_qr_mod[i >> 3] >> (i & 7)) & 1u; }

static void qr_display_cb(esp_qrcode_handle_t qr)
{
    int sz = esp_qrcode_get_size(qr);
    if (sz <= 0 || sz > QR_MAX) { s_qr_size = 0; return; }
    memset(s_qr_mod, 0, sizeof(s_qr_mod));
    for (int y = 0; y < sz; y++)
        for (int x = 0; x < sz; x++)
            if (esp_qrcode_get_module(qr, x, y)) qr_setbit(y * QR_MAX + x);
    s_qr_size = sz;
}

static void generate_qr(const char *text)
{
    s_qr_size = 0;
    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    cfg.display_func       = qr_display_cb;
    cfg.max_qrcode_version = 6;
    cfg.qrcode_ecc_level   = ESP_QRCODE_ECC_LOW;
    esp_err_t e = esp_qrcode_generate(&cfg, text);
    if (e != ESP_OK) ESP_LOGW(TAG, "QR generate failed: %s", esp_err_to_name(e));
}

int  ssh_import_qr_size(void) { return s_qr_size; }
bool ssh_import_qr_module(int x, int y)
{
    if (x < 0 || y < 0 || x >= s_qr_size || y >= s_qr_size) return false;
    return qr_getbit(y * QR_MAX + x);
}

/* ------------------------------------------------------------- HTML form */

/* The proof-code field is emitted between HEAD and TAIL: hidden+prefilled in
 * SOFTAP (the WPA2 join already proved possession), visible+required in WEB
 * (the typed code is the only gate). */
static const char FORM_HEAD[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>Cyberdeck - add SSH profile</title><style>"
    "body{background:#0b0f0b;color:#5f5;font-family:ui-monospace,Menlo,Consolas,monospace;"
    "margin:0;padding:16px;font-size:16px}"
    ".c{max-width:520px;margin:0 auto}"
    "h1{font-size:18px;color:#8f8;border-bottom:1px solid #2a2;padding-bottom:8px}"
    "label{display:block;margin:12px 0 4px;color:#9c9}"
    "input,textarea,select{width:100%;box-sizing:border-box;background:#111;color:#5f5;"
    "border:1px solid #2a2;border-radius:6px;padding:10px;font:inherit}"
    "textarea{height:110px;white-space:pre;overflow:auto}"
    ".row{display:flex;gap:10px}.row>div{flex:1}"
    ".seg{display:flex;gap:8px;margin-top:4px}"
    ".seg label{flex:1;margin:0;text-align:center;border:1px solid #2a2;border-radius:6px;"
    "padding:10px;cursor:pointer}"
    ".seg input{display:none}"
    ".seg label:has(input:checked){background:#5f5;color:#0b0f0b}"
    "button{width:100%;margin-top:18px;background:#5f5;color:#0b0f0b;border:0;border-radius:6px;"
    "padding:14px;font:inherit;font-weight:bold;cursor:pointer}"
    ".hint{color:#686;font-size:13px;margin-top:4px}"
    ".hide{display:none}"
    ".pr{display:flex;gap:8px;align-items:center;margin:8px 0;"
    "border:1px solid #2a2;border-radius:6px;padding:8px}"
    ".pr span{flex:1;word-break:break-all}"
    "button.sm{width:auto;margin:0;padding:8px 10px;font-weight:normal}"
    "button.rd{background:#f66}"
    "</style></head><body><div class=c>"
    "<h1>&#9654; ADD SSH PROFILE</h1>"
    "<form method=post action=/save accept-charset=utf-8>";

static const char FORM_POP_WEB[] =
    "<label>Proof code <span class=hint>(shown on the deck)</span></label>"
    "<input name=pop required maxlength=16 autocomplete=off placeholder=\"16 hex\" "
    "oninput=\"if(this.value.length==16)load()\">";

static const char FORM_TAIL[] =
    "<label>Profile name</label><input name=name maxlength=31 required "
    "placeholder=\"e.g. homelab\">"
    "<div class=row><div><label>Host</label>"
    "<input name=host maxlength=63 required placeholder=\"10.0.0.5 or host\"></div>"
    "<div style=flex:0.4><label>Port</label>"
    "<input name=port type=number value=22 min=1 max=65535></div></div>"
    "<label>Username</label><input name=user maxlength=31 required placeholder=root>"
    "<label>Authentication</label>"
    "<div class=seg>"
    "<label><input type=radio name=auth value=password checked onclick=am()><span>Password</span></label>"
    "<label><input type=radio name=auth value=key onclick=am()><span>Private key</span></label>"
    "</div>"
    "<div id=pw><label>Password</label><input name=password type=password>"
    "<div class=hint>updating a stored profile? blank keeps its password</div></div>"
    "<div id=kb class=hide>"
    "<label>Key</label>"
    "<select name=keyid id=ks onchange=ku()>"
    "<option value=\"\">(paste a new key below)</option></select>"
    "<div id=kn>"
    "<label>Private key (PEM)</label>"
    "<textarea name=key placeholder=\"-----BEGIN OPENSSH PRIVATE KEY-----\"></textarea>"
    "<input type=file onchange=lf(this,'key') class=hint>"
    "<label>Public key <span class=hint>(needed for ed25519/ecdsa)</span></label>"
    "<textarea name=pubkey placeholder=\"ssh-ed25519 AAAA...\" style=height:60px></textarea>"
    "<input type=file onchange=lf(this,'pubkey') class=hint>"
    "</div>"
    "<label>Key passphrase <span class=hint>(if encrypted)</span></label>"
    "<input name=passphrase type=password>"
    "</div>"
    "<button type=submit>SAVE TO DECK</button>"
    "</form>"
    "<h1 style=margin-top:28px>&#9654; ON THE DECK</h1>"
    "<div id=pl class=hint>enter the proof code above to list stored profiles</div>"
    "</div><script>"
    "function am(){var k=document.querySelector('[name=auth]:checked').value=='key';"
    "document.getElementById('kb').className=k?'':'hide';"
    "document.getElementById('pw').className=k?'hide':''}"
    "function lf(i,n){var f=i.files[0];if(!f)return;var r=new FileReader();"
    "r.onload=function(){document.querySelector('[name='+n+']').value=r.result};"
    "r.readAsText(f)}"
    "function ku(){document.getElementById('kn').className="
    "document.getElementById('ks').value?'hide':''}"
    "function pop(){var p=document.querySelector('[name=pop]');return p?p.value:''}"
    "function post(u,b){return fetch(u,{method:'POST',"
    "headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b})}"
    "function load(){if(pop().length<16)return;"
    "post('/list','pop='+encodeURIComponent(pop()))"
    ".then(function(r){return r.ok?r.json():null})"
    ".then(function(d){if(d)render(d)})}"
    "function render(d){"
    "var ks=document.getElementById('ks');while(ks.options.length>1)ks.remove(1);"
    "d.keys.forEach(function(k){var o=document.createElement('option');"
    "o.value=k.id;o.textContent=k.id+(k.type?' ('+k.type+')':'');ks.add(o)});"
    "var pl=document.getElementById('pl');pl.textContent='';pl.className='';"
    "d.profiles.forEach(function(p){"
    "var r=document.createElement('div');r.className='pr';"
    "var t=document.createElement('span');"
    "t.textContent=p.name+' \\u2014 '+p.user+'@'+p.host+':'+p.port+' ['+p.auth+']';"
    "var be=document.createElement('button');be.type='button';be.className='sm';"
    "be.textContent='EDIT';be.onclick=function(){fill(p)};"
    "var bd=document.createElement('button');bd.type='button';bd.className='sm rd';"
    "bd.textContent='DEL';bd.onclick=function(){del(p.name)};"
    "r.appendChild(t);r.appendChild(be);r.appendChild(bd);pl.appendChild(r)});"
    "if(!d.profiles.length){pl.textContent='no stored profiles';pl.className='hint'}}"
    "function fill(p){var f=document.forms[0];"
    "f.name.value=p.name;f.host.value=p.host;f.port.value=p.port;f.user.value=p.user;"
    "f.auth.value=p.auth;am();"
    "if(p.auth=='key'){document.getElementById('ks').value=p.key_id||'';ku()}"
    "f.password.value='';f.passphrase.value='';window.scrollTo(0,0)}"
    "function del(n){if(!confirm('Delete profile \"'+n+'\" from the deck?'))return;"
    "post('/delete','pop='+encodeURIComponent(pop())+'&name='+encodeURIComponent(n))"
    ".then(function(){load()})}"
    "window.addEventListener('load',load);"
    "</script></body></html>";

static esp_err_t ok_page(httpd_req_t *req, const char *msg, const char *sub)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><meta charset=utf-8>"
        "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<style>body{background:#0b0f0b;color:#5f5;font-family:ui-monospace,monospace;"
        "text-align:center;padding:48px 16px}a{color:#8f8}h1{color:#8f8}"
        ".s{color:#686}</style><h1>");
    httpd_resp_sendstr_chunk(req, msg);
    httpd_resp_sendstr_chunk(req, "</h1><p class=s>");
    httpd_resp_sendstr_chunk(req, sub);
    httpd_resp_sendstr_chunk(req, "</p><p><a href=/>&#8592; add another</a></p>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ---------------------------------------------------- form-urlencoded parse */

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* URL-decode src[..slen) into dst (NUL-terminated, capped at dcap). Returns
 * true if the whole input fit; false if it was truncated at the cap. */
static bool url_decode(const char *src, int slen, char *dst, int dcap)
{
    int o = 0;
    for (int i = 0; i < slen; i++) {
        if (o >= dcap - 1) { dst[o] = '\0'; return false; }
        char c = src[i];
        if (c == '+') {
            dst[o++] = ' ';
        } else if (c == '%' && i + 2 < slen) {
            int h = hexval(src[i + 1]), l = hexval(src[i + 2]);
            if (h >= 0 && l >= 0) { dst[o++] = (char)((h << 4) | l); i += 2; }
            else dst[o++] = c;
        } else {
            dst[o++] = c;
        }
    }
    dst[o] = '\0';
    return true;
}

/* Extract field @p name from a form-urlencoded @p body into @p out.
 * Returns 1 if present and fully decoded, 0 if absent, -1 if truncated. */
static int form_field(const char *body, const char *name, char *out, int cap)
{
    int nlen = (int)strlen(name);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, name, nlen) == 0 && p[nlen] == '=') {
            const char *v = p + nlen + 1;
            const char *e = strchr(v, '&');
            int vlen = e ? (int)(e - v) : (int)strlen(v);
            return url_decode(v, vlen, out, cap) ? 1 : -1;
        }
        const char *amp = strchr(p, '&');
        if (!amp) break;
        p = amp + 1;
    }
    out[0] = '\0';
    return 0;
}

/* True if @p s holds a control char (< 0x20). profiles.ini is line-oriented,
 * so a newline in any field would forge a new key/section line — reject it.
 * (The PEM key blob is exempt: it is stored in its own file, not the INI.) */
static bool has_ctrl(const char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s < 0x20) return true;
    return false;
}

/* Fold a profile name into a filesystem-safe key id (alnum/-/_ only). */
static void sanitize_key_id(const char *name, char *out, int cap)
{
    int o = 0;
    for (int i = 0; name[i] && o < cap - 1; i++) {
        char c = name[i];
        if (isalnum((unsigned char)c) || c == '-' || c == '_') out[o++] = c;
        else if (c == ' ')                                     out[o++] = '_';
    }
    out[o] = '\0';
}

static void set_err(const char *msg)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_err, sizeof(s_err), "%s", msg);
    if (s_lock) xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "reject: %s", msg);
}

/* Gate every mutating/reading endpoint on the session code. 64 bits puts
 * the code out of offline reach (gen_code), but the ONLINE path still gets
 * belt and braces: misses back off linearly and POP_FAIL_LOCKOUT misses
 * brick the session (even a correct code is refused) until the user
 * reopens the import modal for a fresh code. Compare is constant-time-ish
 * so timing doesn't leak a prefix match. */
#define POP_FAIL_LOCKOUT 8

static bool check_pop(const char *pop)
{
    if (s_pop_fails >= POP_FAIL_LOCKOUT) {
        ESP_LOGW(TAG, "pop locked out (%d misses)", (int)s_pop_fails);
        return false;
    }
    size_t lp = strlen(pop), ls = strlen(s_pop);
    unsigned diff = (unsigned)(lp ^ ls);
    for (size_t i = 0; i < ls; i++)
        diff |= (unsigned)((i < lp ? pop[i] : 0) ^ s_pop[i]);
    if (diff) {
        s_pop_fails++;
        vTaskDelay(pdMS_TO_TICKS(250u * (unsigned)s_pop_fails));
        return false;
    }
    s_pop_fails = 0;
    return true;
}

/* Write keys/<id>.pub directly (storage_set_key only handles .pem). */
static void write_pubkey(const char *key_id, const char *pub, int len)
{
    char path[160];
    snprintf(path, sizeof(path), "%s/keys/%s.pub",
             storage_platform_mount_point(), key_id);
    FILE *f = fopen(path, "wb");
    if (!f) { ESP_LOGW(TAG, "pubkey open failed"); return; }
    fwrite(pub, 1, (size_t)len, f);
    fclose(f);
}

#define IMPORT_PROFILE_MAX STORAGE_MAX_PROFILES
#define IMPORT_KEY_LIST_MAX 16   /* ids surfaced to the page (incl. orphans) */

/* Ensure @p key_id does not collide with a DIFFERENT profile's key (a
 * many-to-one sanitize would otherwise overwrite an unrelated .pem). Appends a
 * numeric suffix until unique among the loaded set. */
static void unique_key_id(const conn_profile_t *list, int n, const char *name,
                          char *key_id, int cap)
{
    char base[32];
    snprintf(base, sizeof(base), "%s", key_id);
    for (int suffix = 1; suffix < 100; suffix++) {
        bool clash = false;
        for (int i = 0; i < n; i++) {
            if (strcmp(list[i].name, name) == 0) continue;   /* same profile: ok */
            if (list[i].auth == STORAGE_AUTH_KEY &&
                strcmp(list[i].key_id, key_id) == 0) { clash = true; break; }
        }
        if (!clash) return;
        snprintf(key_id, cap, "%.24s_%d", base, suffix + 1);
    }
}

/* ------------------------------------------------------- RAM hygiene
 *
 * Everything this file touches on a save is exactly what the vault exists to
 * protect: the pasted private key, its passphrase, and (via a hydrated
 * storage_load_profiles) every stored login password. It all lands in SPIRAM
 * or on the httpd worker stack, so it must be scrubbed before the memory goes
 * back — plain free() left whole PEMs readable on the external bus
 * (docs/storage_auth.md "RAM hygiene"). Free through these helpers, never
 * free() directly, for anything that has held plaintext.
 * ------------------------------------------------------------------- */

static void wipe_free(void *p, size_t len)
{
    if (!p) return;
    keystore_wipe(p, len);
    free(p);
}

#define PROFILE_SET_BYTES (sizeof(conn_profile_t) * (IMPORT_PROFILE_MAX + 1))

/* Load the current profile set into a fresh SPIRAM array. Passwords come back
 * HYDRATED when the store is unlocked, so the result is secret — release it
 * with free_profile_set(), never free(). */
static conn_profile_t *load_profile_set(int *n)
{
    conn_profile_t *list = heap_caps_malloc(PROFILE_SET_BYTES,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    *n = 0;
    if (list) storage_load_profiles(list, n, IMPORT_PROFILE_MAX);
    return list;
}

static void free_profile_set(conn_profile_t *list)
{
    wipe_free(list, PROFILE_SET_BYTES);
}

/* The secret-bearing locals of handle_save(), grouped into one object so
 * post_save() can scrub them in a SINGLE place rather than before each of a
 * dozen early returns. Deliberately NOT a static: post_save owns it on the
 * stack. The httpd worker's 6 KB is committed whether or not we use it, so
 * borrowing ~750 B of it for the duration of a request costs no resident
 * RAM, whereas .bss would hold that internal SRAM forever for an action the
 * deck performs once in a blue moon. Net stack use is unchanged from having
 * these as plain locals — they just moved one frame up. */
typedef struct {
    char           pop[24];
    char           password[128];
    char           passphrase[128];
    conn_profile_t old;          /* hydrated: carries the stored plaintext */
    conn_profile_t pf;           /* the profile being written              */
} save_scratch_t;

/* Escape @p src into a JSON string-literal body (quotes NOT added). All
 * stored fields are already control-char-free (has_ctrl gates every write),
 * so this mostly guards '"' and '\\'. */
static void json_escape(const char *src, char *dst, int cap)
{
    int o = 0;
    for (int i = 0; src[i] && o < cap - 7; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') { dst[o++] = '\\'; dst[o++] = (char)c; }
        else if (c < 0x20) o += snprintf(dst + o, (size_t)(cap - o),
                                         "\\u%04x", c);
        else dst[o++] = (char)c;
    }
    dst[o] = '\0';
}

/* Append into out[cap] at offset o, SATURATING at cap-1: snprintf returns
 * the would-be length, so a raw "o += snprintf(...)" can push o past cap
 * and turn the next call's (cap - o) into a huge size_t. Saturation keeps
 * every call in bounds and the final overflow check catches truncation. */
static int json_append(char *out, int cap, int o, const char *fmt, ...)
{
    if (o >= cap - 1) return cap - 1;
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(out + o, (size_t)(cap - o), fmt, ap);
    va_end(ap);
    if (r < 0) return cap - 1;
    o += r;
    return o > cap - 1 ? cap - 1 : o;
}

static esp_err_t json_reply(httpd_req_t *req, const char *status,
                            const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

/* Read a small POST body into @p buf. Returns false (nothing sent yet) on
 * empty/oversized/failed reads — caller answers. */
static bool read_small_body(httpd_req_t *req, char *buf, int cap)
{
    int total = req->content_len;
    if (total <= 0 || total >= cap) return false;
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) return false;
        got += r;
    }
    buf[got] = '\0';
    return true;
}

/* Caller MUST be post_save(): it owns @p sc and the single wipe that scrubs
 * it on every exit path out of here. */
static esp_err_t handle_save(httpd_req_t *req, char *body, save_scratch_t *sc)
{
    char name[40], host[80], user[40], port[8], auth[16];
    char keyid[STORAGE_KEY_ID_LEN];

    form_field(body, "pop", sc->pop, sizeof(sc->pop));
    if (!check_pop(sc->pop)) {
        set_err("bad proof code");
        httpd_resp_set_status(req, "403 Forbidden");
        return ok_page(req, "&#10007; Rejected", "Proof code did not match.");
    }

    form_field(body, "name", name, sizeof(name));
    form_field(body, "host", host, sizeof(host));
    form_field(body, "user", user, sizeof(user));
    form_field(body, "port", port, sizeof(port));
    form_field(body, "auth", auth, sizeof(auth));
    form_field(body, "password",   sc->password,   sizeof(sc->password));
    form_field(body, "passphrase", sc->passphrase, sizeof(sc->passphrase));
    form_field(body, "keyid",      keyid,             sizeof(keyid));

    if (!name[0] || !host[0] || !user[0]) {
        set_err("name, host and user are required");
        httpd_resp_set_status(req, "400 Bad Request");
        return ok_page(req, "&#10007; Missing fields",
                       "Name, host and username are required.");
    }
    /* INI-safety: no control chars (would forge a line) in any INI field, and
     * no [ ] in the name (would forge a section header). */
    if (has_ctrl(name) || has_ctrl(host) || has_ctrl(user) ||
        has_ctrl(sc->password) || has_ctrl(sc->passphrase) ||
        has_ctrl(keyid)) {
        set_err("control characters not allowed");
        httpd_resp_set_status(req, "400 Bad Request");
        return ok_page(req, "&#10007; Bad input",
                       "Fields cannot contain control characters.");
    }
    if (strpbrk(name, "[]")) {
        set_err("name cannot contain [ or ]");
        httpd_resp_set_status(req, "400 Bad Request");
        return ok_page(req, "&#10007; Bad name", "Name cannot contain [ or ].");
    }
    int portn = port[0] ? atoi(port) : 22;
    if (portn < 1 || portn > 65535) {
        set_err("port out of range");
        httpd_resp_set_status(req, "400 Bad Request");
        return ok_page(req, "&#10007; Bad port", "Port must be 1-65535.");
    }

    /* Load the set once: same-name slot lookup (replace semantics + inherit
     * rules), capacity check, and key-id collision resolution all use it. */
    int n = 0;
    conn_profile_t *list = load_profile_set(&n);
    if (!list) {
        set_err("out of memory");
        httpd_resp_set_status(req, "500 Internal Server Error");
        return ok_page(req, "&#10007; Out of memory", "The deck is low on RAM.");
    }
    int slot = -1;
    for (int i = 0; i < n; i++)
        if (strcmp(list[i].name, name) == 0) { slot = i; break; }
    if (slot >= 0) sc->old = list[slot];

    /* A brand-new name at capacity fails BEFORE any key is written. */
    if (slot < 0 && n >= IMPORT_PROFILE_MAX) {
        free_profile_set(list);
        set_err("profile list full");
        httpd_resp_set_status(req, "507 Insufficient Storage");
        return ok_page(req, "&#10007; List full",
                       "The deck already holds the maximum profiles.");
    }

    conn_profile_t *pf = &sc->pf;
    snprintf(pf->name, sizeof(pf->name), "%s", name);
    snprintf(pf->host, sizeof(pf->host), "%s", host);
    snprintf(pf->user, sizeof(pf->user), "%s", user);
    pf->port = (uint16_t)portn;

    bool want_key = (strcmp(auth, "key") == 0);
    if (want_key) {
        pf->auth = STORAGE_AUTH_KEY;
        /* password field doubles as the key passphrase (see cyberdeck_app);
         * on a same-name update a blank passphrase keeps the stored one. */
        snprintf(pf->password, sizeof(pf->password), "%s", sc->passphrase);
        if (!pf->password[0] && sc->old.auth == STORAGE_AUTH_KEY)
            snprintf(pf->password, sizeof(pf->password), "%s",
                     sc->old.password);

        char *key    = heap_caps_malloc(BODY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        char *pubkey = heap_caps_malloc(4096,     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!key || !pubkey) {
            wipe_free(key, BODY_MAX); free(pubkey); free_profile_set(list);
            set_err("out of memory for key");
            httpd_resp_set_status(req, "500 Internal Server Error");
            return ok_page(req, "&#10007; Out of memory", "Try a smaller key.");
        }
        int kr = form_field(body, "key",    key,    BODY_MAX);
        int pr = form_field(body, "pubkey", pubkey, 4096);
        if (kr < 0 || pr < 0) {          /* decoded value hit the buffer cap */
            wipe_free(key, BODY_MAX); free(pubkey); free_profile_set(list);
            set_err("key too large");
            httpd_resp_set_status(req, "413 Payload Too Large");
            return ok_page(req, "&#10007; Key too large", "The key did not fit.");
        }

        if (keyid[0]) {
            /* Reference an already-stored key. The dropdown hides the paste
             * box, so a stale pasted blob must not win over the selection.
             * (ids on the heap — the httpd worker stack is only 6144 B.) */
            char (*ids)[STORAGE_KEY_ID_LEN] =
                heap_caps_malloc(IMPORT_KEY_LIST_MAX * STORAGE_KEY_ID_LEN,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            bool known = false;
            if (ids) {
                int nk = 0;
                storage_list_keys(ids, IMPORT_KEY_LIST_MAX, &nk);
                for (int i = 0; i < nk; i++)
                    if (strcmp(ids[i], keyid) == 0) { known = true; break; }
                free(ids);
            }
            wipe_free(key, BODY_MAX); free(pubkey);
            if (!known) {
                free_profile_set(list);
                set_err("unknown key id");
                httpd_resp_set_status(req, "400 Bad Request");
                return ok_page(req, "&#10007; Unknown key",
                               "That stored key no longer exists - reload.");
            }
            snprintf(pf->key_id, sizeof(pf->key_id), "%s", keyid);
        } else if (key[0]) {
            /* Upload a new key blob. */
            if (!strstr(key, "PRIVATE KEY")) {
                wipe_free(key, BODY_MAX); free(pubkey); free_profile_set(list);
                set_err("not a PEM private key");
                httpd_resp_set_status(req, "400 Bad Request");
                return ok_page(req, "&#10007; Bad key",
                               "Paste a PEM private key (BEGIN ... PRIVATE KEY).");
            }
            char key_id[32];
            sanitize_key_id(name, key_id, sizeof(key_id));
            if (!key_id[0]) {
                wipe_free(key, BODY_MAX); free(pubkey); free_profile_set(list);
                set_err("name has no usable characters");
                httpd_resp_set_status(req, "400 Bad Request");
                return ok_page(req, "&#10007; Bad name",
                               "Name needs a letter or digit.");
            }
            /* Resolve key-id collisions against OTHER profiles before writing. */
            unique_key_id(list, n, name, key_id, sizeof(key_id));

            esp_err_t e = storage_set_key(key_id, key, strlen(key));
            if (e == ESP_OK && pubkey[0])
                write_pubkey(key_id, pubkey, (int)strlen(pubkey));
            wipe_free(key, BODY_MAX); free(pubkey);
            if (e != ESP_OK) {
                free_profile_set(list);
                set_err("failed to store key");
                httpd_resp_set_status(req, "500 Internal Server Error");
                return ok_page(req, "&#10007; Store failed",
                               "Could not write the key.");
            }
            snprintf(pf->key_id, sizeof(pf->key_id), "%s", key_id);
        } else {
            /* No key material at all: an update may keep the stored key. */
            wipe_free(key, BODY_MAX); free(pubkey);
            if (sc->old.auth != STORAGE_AUTH_KEY || !sc->old.key_id[0]) {
                free_profile_set(list);
                set_err("key required");
                httpd_resp_set_status(req, "400 Bad Request");
                return ok_page(req, "&#10007; Key required",
                               "Pick a stored key or paste a new one.");
            }
            snprintf(pf->key_id, sizeof(pf->key_id), "%s", sc->old.key_id);
        }
    } else {
        pf->auth = STORAGE_AUTH_PASSWORD;
        snprintf(pf->password, sizeof(pf->password), "%s", sc->password);
        /* Same-name update with a blank password keeps the stored one. */
        if (!pf->password[0] && sc->old.auth == STORAGE_AUTH_PASSWORD)
            snprintf(pf->password, sizeof(pf->password), "%s",
                     sc->old.password);
    }

    if (slot < 0) slot = n++;
    list[slot] = *pf;
    esp_err_t ae = storage_save_profiles(list, n);
    if (ae != ESP_OK) {
        free_profile_set(list);
        set_err("failed to save profile");
        httpd_resp_set_status(req, "500 Internal Server Error");
        return ok_page(req, "&#10007; Save failed", "Could not write profiles.");
    }

    /* The update dropped or swapped this profile's key: GC the orphan .pem
     * when no profile references it anymore (mirrors the on-device editor). */
    if (sc->old.auth == STORAGE_AUTH_KEY && sc->old.key_id[0]) {
        bool shared = false;
        for (int i = 0; i < n; i++)
            if (list[i].auth == STORAGE_AUTH_KEY &&
                strcmp(list[i].key_id, sc->old.key_id) == 0) {
                shared = true; break;
            }
        if (!shared) storage_delete_key(sc->old.key_id);
    }
    free_profile_set(list);

    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_err[0] = '\0';
    snprintf(s_last, sizeof(s_last), "%s", pf->name);
    if (s_lock) xSemaphoreGive(s_lock);
    s_count++;                                   /* publish after s_last is set */
    ESP_LOGI(TAG, "imported profile '%s' (%s)", pf->name, want_key ? "key" : "password");

    char sub[80];
    snprintf(sub, sizeof(sub), "%s @ %s:%d saved to the deck.",
             pf->user, pf->host, portn);
    return ok_page(req, "&#10003; Saved", sub);
}

/* POST /list — pop-gated JSON of stored profiles + key ids (no secrets:
 * passwords, passphrases and key material never leave the deck). */
static esp_err_t post_list(httpd_req_t *req)
{
    char body[64], pop[24];
    if (!read_small_body(req, body, sizeof(body)))
        return json_reply(req, "400 Bad Request", "{\"ok\":false}");
    form_field(body, "pop", pop, sizeof(pop));
    bool authed = check_pop(pop);
    keystore_wipe(pop, sizeof(pop));      /* session credential, done with it */
    keystore_wipe(body, sizeof(body));    /* nothing below reads the body     */
    if (!authed)
        return json_reply(req, "403 Forbidden", "{\"ok\":false}");

    enum { OUT_CAP = 8192 };
    int n = 0;
    conn_profile_t *list = load_profile_set(&n);
    char *out = heap_caps_malloc(OUT_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char (*ids)[STORAGE_KEY_ID_LEN] =
        heap_caps_malloc(IMPORT_KEY_LIST_MAX * STORAGE_KEY_ID_LEN,
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!list || !out || !ids) {
        free_profile_set(list); free(out); free(ids);
        return json_reply(req, "500 Internal Server Error", "{\"ok\":false}");
    }

    int o = 0;
    o = json_append(out, OUT_CAP, o, "{\"max\":%d,\"profiles\":[",
                    IMPORT_PROFILE_MAX);
    char e1[80], e2[160];
    for (int i = 0; i < n && o < OUT_CAP - 2; i++) {
        json_escape(list[i].name, e1, sizeof(e1));
        json_escape(list[i].host, e2, sizeof(e2));
        o = json_append(out, OUT_CAP, o,
                        "%s{\"name\":\"%s\",\"host\":\"%s\",\"port\":%u",
                        i ? "," : "", e1, e2, (unsigned)list[i].port);
        json_escape(list[i].user, e1, sizeof(e1));
        json_escape(list[i].key_id, e2, sizeof(e2));
        o = json_append(out, OUT_CAP, o,
                        ",\"user\":\"%s\",\"auth\":\"%s\",\"key_id\":\"%s\"}",
                        e1, list[i].auth == STORAGE_AUTH_KEY ? "key" : "password",
                        list[i].auth == STORAGE_AUTH_KEY ? e2 : "");
    }
    free_profile_set(list);

    o = json_append(out, OUT_CAP, o, "],\"keys\":[");
    int nk = 0;
    storage_list_keys(ids, IMPORT_KEY_LIST_MAX, &nk);
    for (int i = 0; i < nk && o < OUT_CAP - 2; i++) {
        char type[24], comment[64];
        storage_key_info(ids[i], type, sizeof(type), comment, sizeof(comment));
        json_escape(ids[i], e1, sizeof(e1));
        o = json_append(out, OUT_CAP, o, "%s{\"id\":\"%s\"", i ? "," : "", e1);
        json_escape(type, e1, sizeof(e1));
        json_escape(comment, e2, sizeof(e2));
        o = json_append(out, OUT_CAP, o,
                        ",\"type\":\"%s\",\"comment\":\"%s\"}", e1, e2);
    }
    free(ids);
    o = json_append(out, OUT_CAP, o, "]}");

    esp_err_t e;
    if (o >= OUT_CAP - 2) {           /* truncated mid-structure: bail out */
        e = json_reply(req, "500 Internal Server Error", "{\"ok\":false}");
    } else {
        httpd_resp_set_type(req, "application/json");
        e = httpd_resp_sendstr(req, out);
    }
    free(out);
    return e;
}

/* POST /delete — pop-gated removal by profile name; reclaims the key files
 * and the TOFU host pin when no surviving profile references them. */
static esp_err_t post_delete(httpd_req_t *req)
{
    char body[256];
    if (!read_small_body(req, body, sizeof(body)))
        return json_reply(req, "400 Bad Request", "{\"ok\":false}");
    char pop[24], name[40];
    form_field(body, "pop", pop, sizeof(pop));
    bool authed = check_pop(pop);
    form_field(body, "name", name, sizeof(name));   /* last read of the body */
    keystore_wipe(pop, sizeof(pop));
    keystore_wipe(body, sizeof(body));
    if (!authed) {
        set_err("bad proof code");
        return json_reply(req, "403 Forbidden", "{\"ok\":false}");
    }
    if (!name[0])
        return json_reply(req, "400 Bad Request", "{\"ok\":false}");

    int n = 0;
    conn_profile_t *list = load_profile_set(&n);
    if (!list)
        return json_reply(req, "500 Internal Server Error", "{\"ok\":false}");

    int idx = -1;
    for (int i = 0; i < n; i++)
        if (strcmp(list[i].name, name) == 0) { idx = i; break; }
    if (idx < 0) {
        free_profile_set(list);
        return json_reply(req, "404 Not Found", "{\"ok\":false}");
    }
    conn_profile_t doomed = list[idx];      /* hydrated: wiped before return */
    for (int i = idx; i < n - 1; i++) list[i] = list[i + 1];
    n--;

    esp_err_t e = storage_save_profiles(list, n);
    if (e == ESP_OK) {
        if (doomed.auth == STORAGE_AUTH_KEY && doomed.key_id[0]) {
            bool shared = false;
            for (int i = 0; i < n; i++)
                if (list[i].auth == STORAGE_AUTH_KEY &&
                    strcmp(list[i].key_id, doomed.key_id) == 0) { shared = true; break; }
            if (!shared) storage_delete_key(doomed.key_id);
        }
        bool host_shared = false;
        for (int i = 0; i < n; i++)
            if (list[i].port == doomed.port &&
                strcmp(list[i].host, doomed.host) == 0) { host_shared = true; break; }
        if (!host_shared) storage_known_host_delete(doomed.host, doomed.port);
    }
    free_profile_set(list);
    keystore_wipe(&doomed, sizeof(doomed));

    if (e != ESP_OK) {
        set_err("failed to save profiles");
        return json_reply(req, "500 Internal Server Error", "{\"ok\":false}");
    }
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_err[0] = '\0';
    snprintf(s_last, sizeof(s_last), "%s", name);
    if (s_lock) xSemaphoreGive(s_lock);
    s_deleted++;                                 /* publish after s_last is set */
    ESP_LOGI(TAG, "deleted profile '%s' via web", name);
    return json_reply(req, "200 OK", "{\"ok\":true}");
}

/* ------------------------------------------------------------- HTTP handlers */

/* Must the browser user type the proof code? WEB mode only — in SoftAP the
 * WPA2 join is the proof and the field is hidden+prefilled. The DEV Kconfig
 * toggle prefills it in WEB mode too (the page then carries the code, so
 * the gate is effectively off for anyone who can load the page). */
bool ssh_import_pop_required(void)
{
#ifdef CONFIG_SSH_IMPORT_POP_DISABLED
    return false;
#else
    return s_mode == SSH_IMPORT_WEB;
#endif
}

static esp_err_t get_form(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, FORM_HEAD);
    if (ssh_import_pop_required()) {
        httpd_resp_sendstr_chunk(req, FORM_POP_WEB);    /* visible, user types it */
    } else {
        httpd_resp_sendstr_chunk(req, "<input type=hidden name=pop value=\"");
        httpd_resp_sendstr_chunk(req, s_pop);           /* prefilled */
        httpd_resp_sendstr_chunk(req, "\">");
    }
    httpd_resp_sendstr_chunk(req, FORM_TAIL);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t post_save(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return ok_page(req, "&#10007; Empty", "No form data received.");
    }
    if (total > BODY_MAX) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return ok_page(req, "&#10007; Too large", "Payload exceeds 16 KB.");
    }

    char *body = heap_caps_malloc(total + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return ok_page(req, "&#10007; Out of memory", "The deck is low on RAM.");
    }

    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0) {                     /* partial body still holds secrets */
            wipe_free(body, (size_t)total + 1);
            return ESP_FAIL;
        }
        got += r;
    }
    body[got] = '\0';

    /* esp_http_server processes requests on a single worker task, so handle_save
     * never runs concurrently — no outer lock needed (and holding s_lock here
     * would self-deadlock, since set_err()/the success path re-take it). s_lock
     * guards only the s_last/s_err strings shared with the render task. */
    /* The body is the rawest copy of everything secret in this request — the
     * pasted PEM, its passphrase, the login password, the proof code — so it
     * gets scrubbed, not just freed. The scratch is owned here for the same
     * reason: one wipe covers every early return inside handle_save(). */
    save_scratch_t sc;
    memset(&sc, 0, sizeof(sc));
    esp_err_t e = handle_save(req, body, &sc);
    wipe_free(body, (size_t)total + 1);
    keystore_wipe(&sc, sizeof(sc));
    return e;
}

static esp_err_t start_httpd(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 6;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
    cfg.stack_size       = 6144;

    esp_err_t e = httpd_start(&s_httpd, &cfg);
    if (e != ESP_OK) { s_httpd = NULL; return e; }

    httpd_uri_t save = { .uri = "/save",   .method = HTTP_POST, .handler = post_save };
    httpd_uri_t list = { .uri = "/list",   .method = HTTP_POST, .handler = post_list };
    httpd_uri_t del  = { .uri = "/delete", .method = HTTP_POST, .handler = post_delete };
    httpd_uri_t form = { .uri = "/*",      .method = HTTP_GET,  .handler = get_form };
    httpd_register_uri_handler(s_httpd, &save);
    httpd_register_uri_handler(s_httpd, &list);
    httpd_register_uri_handler(s_httpd, &del);
    httpd_register_uri_handler(s_httpd, &form);
    return ESP_OK;
}

/* -------------------------------------------------------------- lifecycle */

/* 16 random hex chars = a fresh per-session proof code (also a valid WPA2 key,
 * which needs 8..63 ASCII). Two esp_random() draws, not one: in SOFTAP mode
 * this string IS the WPA2 passphrase, and a single 32-bit draw put a captured
 * handshake inside a 2^32 offline search — hours on a GPU, after which the
 * attacker owns profile writes and can plant a key. 64 bits takes that off
 * the table for the cost of eight more characters to type.
 * Not MAC-derived: MAC bytes leak in the SSID + AP beacon, which would let a
 * passive listener reconstruct the PSK and decrypt the SoftAP link. */
static void gen_code(void)
{
    uint32_t hi = esp_random(), lo = esp_random();
    snprintf(s_pop, sizeof(s_pop), "%08lX%08lX",
             (unsigned long)hi, (unsigned long)lo);
}

static esp_err_t start_softap(void)
{
    wifi_manager_disconnect();          /* park STA retry loop for the session */
    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();

    uint8_t mac[6] = { 0 };
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(s_service, sizeof(s_service), "DECK-SETUP-%02X%02X", mac[4], mac[5]);
    snprintf(s_url,     sizeof(s_url),     "http://192.168.4.1");

    wifi_config_t ap = { 0 };
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", s_service);
    ap.ap.ssid_len         = (uint8_t)strlen(s_service);
    snprintf((char *)ap.ap.password, sizeof(ap.ap.password), "%s", s_pop);
    ap.ap.channel          = 1;
    ap.ap.max_connection   = 1;
    ap.ap.authmode         = WIFI_AUTH_WPA2_PSK;
    ap.ap.pmf_cfg.required = false;

    esp_err_t e = esp_wifi_set_mode(WIFI_MODE_AP);
    if (e == ESP_OK) e = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (e == ESP_OK) e = esp_wifi_start();       /* propagate: no live AP = fail */
    if (e != ESP_OK) { ESP_LOGE(TAG, "SoftAP up: %s", esp_err_to_name(e)); return e; }

    char qr[96];
    snprintf(qr, sizeof(qr), "WIFI:T:WPA;S:%s;P:%s;;", s_service, s_pop);
    generate_qr(qr);
    ESP_LOGI(TAG, "import AP up: join '%s' (key '%s'), open %s",
             s_service, s_pop, s_url);
    return ESP_OK;
}

static esp_err_t start_web(void)
{
    const char *ip = wifi_manager_get_ip();
    if (!ip || !ip[0]) {
        ESP_LOGW(TAG, "web import needs an active WiFi (STA) link");
        return ESP_ERR_INVALID_STATE;
    }
    s_service[0] = '\0';                 /* no SSID in web mode */
    snprintf(s_url, sizeof(s_url), "http://%s", ip);
    generate_qr(s_url);                  /* URL QR (handy for a phone too) */
    ESP_LOGI(TAG, "web import up: open %s (code %s)", s_url, s_pop);
    return ESP_OK;
}

esp_err_t ssh_import_start(ssh_import_mode_t mode)
{
    if (s_state != SSH_IMPORT_ST_IDLE) return ESP_ERR_INVALID_STATE;

    if (wifi_manager_init() != ESP_OK) return ESP_FAIL;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();

    s_mode = mode;
    s_last[0] = s_err[0] = '\0';
    s_count     = 0;
    s_deleted   = 0;
    s_pop_fails = 0;
    gen_code();

    esp_err_t e = (mode == SSH_IMPORT_WEB) ? start_web() : start_softap();
    if (e != ESP_OK) { ssh_import_stop(); return e; }   /* stop() leaves IDLE */

    if (start_httpd() != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        ssh_import_stop();
        return ESP_FAIL;
    }

    s_state = SSH_IMPORT_ST_ACTIVE;
    return ESP_OK;
}

void ssh_import_stop(void)
{
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }

    if (s_mode == SSH_IMPORT_SOFTAP) {
        esp_wifi_set_mode(WIFI_MODE_STA);   /* drop the AP radio, back to STA */
        if (s_ap_netif) {
            esp_netif_destroy_default_wifi(s_ap_netif);
            s_ap_netif = NULL;
        }
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta) esp_netif_set_default_netif(sta);
    }
    /* WEB mode never touched the STA link — nothing to restore. */

    s_state   = SSH_IMPORT_ST_IDLE;
    s_qr_size = 0;
    s_service[0] = s_url[0] = '\0';
    ESP_LOGI(TAG, "import stopped");
}

int         ssh_import_state(void)        { return s_state; }
int         ssh_import_mode(void)         { return s_mode; }
const char *ssh_import_service_name(void) { return s_service; }
const char *ssh_import_pop(void)          { return s_pop; }
const char *ssh_import_url(void)          { return s_url; }
int         ssh_import_count(void)        { return s_count; }
int         ssh_import_deleted(void)      { return s_deleted; }

/* Status strings are written by the httpd task under s_lock; copy under the
 * same lock into a small static so the render task never reads a torn string. */
const char *ssh_import_last(void)
{
    static char snap[32];
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(snap, sizeof(snap), "%s", s_last);
    if (s_lock) xSemaphoreGive(s_lock);
    return snap;
}

const char *ssh_import_err(void)
{
    static char snap[64];
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(snap, sizeof(snap), "%s", s_err);
    if (s_lock) xSemaphoreGive(s_lock);
    return snap;
}
