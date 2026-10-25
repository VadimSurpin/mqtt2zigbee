#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "cJSON.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "web_server.h"
#include "web_ui.h"
#include "app_config.h"
#include "wifi_manager.h"
#include "mqtt_client_wrapper.h"
#include "wb_driver.h"
#include "z2m_driver.h"
#include "locale.h"
#include "zigbee_coordinator.h"

static const char *TAG = "WEB_SERVER";

static bool s_needs_restart = false;

/* ── HTML output buffer ──────────────────────────────────────────────────── */
static char s_html[16384];
static int  s_html_len;

static void html_reset(void) { s_html_len = 0; s_html[0] = '\0'; }

static void html_append(const char *s)
{
    int l = (int)strlen(s);
    if (s_html_len + l < (int)sizeof(s_html) - 1) {
        memcpy(s_html + s_html_len, s, l);
        s_html_len += l;
        s_html[s_html_len] = '\0';
    }
}

static void html_appendf(const char *fmt, ...)
{
    int avail = (int)sizeof(s_html) - s_html_len - 1;
    if (avail <= 0) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s_html + s_html_len, avail, fmt, ap);
    va_end(ap);
    /* Cap at bytes actually written — vsnprintf returns would-have-written, not did-write. */
    if (n > 0) s_html_len += (n < avail ? n : avail);
}

#define HS(s)        html_append(s)
#define HF(fmt, ...) html_appendf(fmt, ##__VA_ARGS__)

/* ── Session ─────────────────────────────────────────────────────────────── */
static char s_session_token[33] = {0};

static void generate_session(void)
{
    uint8_t raw[16];
    esp_fill_random(raw, sizeof(raw));
    for (int i = 0; i < 16; i++)
        snprintf(s_session_token + i * 2, 3, "%02x", raw[i]);
}

static void clear_session(void) { s_session_token[0] = '\0'; }

/* ── Pair state ──────────────────────────────────────────────────────────── */
#define PAIR_WINDOW_US  (60LL * 1000000LL)

static int64_t s_pair_open_until_us = 0;
static char    s_paired_name[64]    = {0};
static char    s_paired_ieee[32]    = {0};
static char    s_paired_model[64]   = {0};

/* ── Zigbee info (set externally by coordinator) ────────────────────────── */
static char     s_zb_ieee[32]   = {0};
static uint16_t s_zb_pan_id     = 0;
static uint8_t  s_zb_channel    = 0;
static uint16_t s_zb_short_addr = 0;

/* ── WiFi scan state ─────────────────────────────────────────────────────── */
static bool            s_scan_done = false;
static wifi_ap_record_t s_ap_records[20];
static uint16_t        s_ap_count  = 0;

/* ── Form body buffer ───────────────────────────────────────────────────── */
static char s_form_buf[2048];

static httpd_handle_t s_server = NULL;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void url_decode(char *dst, const char *src, size_t dst_size)
{
    char *d = dst;
    while (*src && d < dst + dst_size - 1) {
        if (*src == '%' && *(src + 1) && *(src + 2)) {
            char hex[3] = {*(src + 1), *(src + 2), 0};
            *d++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *d++ = ' ';
            src++;
        } else {
            *d++ = *src++;
        }
    }
    *d = '\0';
}

static bool form_read(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(s_form_buf) - 1) {
        s_form_buf[0] = '\0';
        return false;
    }
    int ret = httpd_req_recv(req, s_form_buf, total);
    if (ret <= 0) { s_form_buf[0] = '\0'; return false; }
    s_form_buf[ret] = '\0';
    return true;
}

static bool form_get(const char *body, const char *key, char *out, size_t out_size)
{
    char search[64];
    snprintf(search, sizeof(search), "%s=", key);
    const char *p = strstr(body, search);
    if (!p) { out[0] = '\0'; return false; }
    p += strlen(search);
    char raw[512];
    size_t i = 0;
    while (*p && *p != '&' && i < sizeof(raw) - 1) raw[i++] = *p++;
    raw[i] = '\0';
    url_decode(out, raw, out_size);
    return true;
}

static bool get_session_cookie(httpd_req_t *req, char *out, size_t out_size)
{
    char hdr[512] = {0};
    if (httpd_req_get_hdr_value_str(req, "Cookie", hdr, sizeof(hdr)) != ESP_OK)
        return false;
    const char *p = strstr(hdr, "session=");
    if (!p) return false;
    p += 8;
    size_t i = 0;
    while (*p && *p != ';' && i < out_size - 1) out[i++] = *p++;
    out[i] = '\0';
    return i > 0;
}

static bool is_authenticated(httpd_req_t *req)
{
    if (!s_session_token[0]) return false;
    char tok[33] = {0};
    return get_session_cookie(req, tok, sizeof(tok))
        && strcmp(tok, s_session_token) == 0;
}

static bool is_default_creds(void)
{
    return strcmp(app_config_get_ui_user(), "admin") == 0
        && strcmp(app_config_get_ui_pass(), "gateway") == 0;
}

static esp_err_t send_redirect(httpd_req_t *req, const char *loc)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", loc);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, "", 0);
    return ESP_OK;
}

static esp_err_t send_html(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, s_html, s_html_len);
    return ESP_OK;
}

/* ── Common CSS & navigation ─────────────────────────────────────────────── */
static const char COMMON_CSS[] =
    "body{background:#f4f5f7}"
    ".card{border-radius:12px;box-shadow:0 2px 12px rgba(0,0,0,.1);border:none}"
    ".card-header{background:linear-gradient(135deg,#667eea,#764ba2);color:#fff;"
    "border-radius:12px 12px 0 0!important;padding:.75rem 1.25rem}"
    ".wifi-item{cursor:pointer;transition:background .15s;border:1px solid #dee2e6}"
    ".wifi-item:hover{background:#f0f0ff}"
    ".wifi-item.chosen{background:#e8eeff;border-color:#667eea!important}"
    ".status-badge{font-size:.8rem;padding:.35em .7em}"
    ".nav-link.active{font-weight:600}";

static const struct { const char *path; const char *icon; locale_key_t label_key; }
DASH_PAGES[] = {
    {"/status",          "bi-speedometer2",    L_NAV_STATUS},
    {"/wifi",            "bi-wifi",            L_NAV_WIFI},
    {"/mqtt",            "bi-hdd-network",     L_NAV_MQTT},
    {"/devices",         "bi-diagram-3",       L_NAV_DEVICES},
    {"/pair",            "bi-broadcast",       L_NAV_PAIR},
    {"/backup",          "bi-download",        L_NAV_BACKUP},
    {"/firmware",        "bi-arrow-up-circle", L_NAV_FIRMWARE},
    {"/change-password", "bi-key",             L_NAV_SECURITY},
};
#define DASH_PAGES_COUNT (sizeof(DASH_PAGES) / sizeof(DASH_PAGES[0]))

static void page_common_head(const char *title)
{
    html_reset();
    HS("<!DOCTYPE html><html lang=\"en\"><head>"
       "<meta charset=\"UTF-8\">"
       "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
    HF("<title>%s \xe2\x80\x93 Zigbee Gateway</title>", title);
    HS("<link rel=\"stylesheet\" href=\"/css/bootstrap.min.css\">"
       "<link rel=\"stylesheet\" href=\"/css/bootstrap-icons.css\">"
       "<style>");
    HS(COMMON_CSS);
    HS("</style></head><body>");
}

static void dash_page_open(const char *title, const char *active_path)
{
    page_common_head(title);
    lang_t cur_lang = locale_get();
    /* Two-row navbar: row 1 = brand + controls, row 2 = nav links */
    HS("<nav class=\"navbar navbar-light bg-white border-bottom mb-4 px-2\">"
       "<div class=\"container-fluid flex-column align-items-start gap-1 py-1\""
       " style=\"max-width:640px\">"
       "<div class=\"d-flex align-items-center justify-content-between w-100\">"
       "<span class=\"fw-bold small\">[Z] Zigbee MQTT Gateway</span>"
       "<div class=\"d-flex align-items-center gap-1\">"
       "<form method=\"post\" action=\"/logout\" class=\"mb-0\">"
       "<button class=\"btn btn-sm btn-outline-secondary py-0 px-2\">"
       "<i class=\"bi bi-box-arrow-right me-1\"></i>");
    HS(t(L_BTN_LOGOUT));
    HS("</button></form>"
       "<form method=\"post\" action=\"/lang\" class=\"mb-0 d-flex gap-1\">");
    HF("<button name=\"lang\" value=\"en\" class=\"btn btn-sm py-0 px-2 %s\">EN</button>",
       cur_lang == LANG_EN ? "btn-primary" : "btn-outline-secondary");
    HF("<button name=\"lang\" value=\"ru\" class=\"btn btn-sm py-0 px-2 %s\">RU</button>",
       cur_lang == LANG_RU ? "btn-primary" : "btn-outline-secondary");
    HS("</form></div></div>"
       "<div class=\"d-flex flex-wrap gap-1 w-100\">");
    for (size_t i = 0; i < DASH_PAGES_COUNT; i++) {
        bool active = strcmp(DASH_PAGES[i].path, active_path) == 0;
        HF("<a href=\"%s\" class=\"nav-link px-2 py-1%s\">",
           DASH_PAGES[i].path, active ? " active" : "");
        HF("<i class=\"bi %s me-1\"></i>%s</a>",
           DASH_PAGES[i].icon, t(DASH_PAGES[i].label_key));
    }
    HS("</div></div></nav>"
       "<div class=\"container pb-4\" style=\"max-width:640px\">");
    if (s_needs_restart) {
        HS("<div class=\"alert alert-warning d-flex align-items-center justify-content-between py-2 mb-3\">"
           "<span><i class=\"bi bi-exclamation-triangle-fill me-2\"></i><strong>");
        HS(t(L_RESTART_REQUIRED));
        HS("</strong> — ");
        HS(t(L_RESTART_MSG));
        HS("</span>"
           "<form method=\"post\" action=\"/restart\" class=\"mb-0 ms-3 flex-shrink-0\">"
           "<button class=\"btn btn-warning btn-sm\">");
        HS(t(L_BTN_RESTART_NOW));
        HS("</button></form></div>");
    }
}

static void dash_page_close(void)
{
    HS("</div><script src=\"/js/enhance.js\"></script></body></html>");
}

static void auth_page_open(const char *title)
{
    page_common_head(title);
    HS("<div class=\"container py-5\" style=\"max-width:400px\">"
       "<div class=\"text-center mb-4\">"
       "<h5 class=\"fw-bold mb-0\">"
       "<i class=\"bi bi-broadcast me-2\"></i>Zigbee MQTT Gateway</h5>"
       "</div>");
}

static void auth_page_close(void) { HS("</div></body></html>"); }

static void alert_div(const char *msg, const char *kind)
{
    if (msg && msg[0])
        HF("<div class=\"alert alert-%s py-2 small mb-3\">%s</div>", kind, msg);
}

/* ── Static file handlers ────────────────────────────────────────────────── */

static esp_err_t favicon_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, "", 0);
    return ESP_OK;
}

static esp_err_t css_bootstrap_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/css");
    httpd_resp_send(req, web_ui_get_bootstrap_css_start(),
                    web_ui_get_bootstrap_css_size());
    return ESP_OK;
}

static esp_err_t css_icons_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/css");
    httpd_resp_send(req, web_ui_get_bootstrap_icons_css_start(),
                    web_ui_get_bootstrap_icons_css_size());
    return ESP_OK;
}

static esp_err_t font_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "font/woff2");
    httpd_resp_send(req, web_ui_get_font_woff2_start(),
                    web_ui_get_font_woff2_size());
    return ESP_OK;
}

static esp_err_t enhance_js_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_send(req, web_ui_get_enhance_js_start(),
                    web_ui_get_enhance_js_size());
    return ESP_OK;
}

/* ── Device entry string helpers ─────────────────────────────────────────── */
/* Format: "type|topic[|mfr[|model]]"  All helpers work on all variants.     */

static const char *entry_topic_ptr(const char *entry)
{
    const char *p = strchr(entry, '|');
    return p ? p + 1 : entry;
}

static void entry_get_topic(const char *entry, char *out, size_t out_sz)
{
    const char *start = entry_topic_ptr(entry);
    const char *end   = strchr(start, '|');
    size_t len = end ? (size_t)(end - start) : strlen(start);
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, start, len);
    out[len] = '\0';
}

/* Copies the device type (first pipe-segment). */
static void entry_get_type(const char *entry, char *out, size_t out_sz)
{
    const char *p = strchr(entry, '|');
    size_t len = p ? (size_t)(p - entry) : strlen(entry);
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, entry, len);
    out[len] = '\0';
}

/* Returns true if entry starts with "type|topic" (handles 2- or 4-part format). */
static bool entry_matches_base(const char *entry, const char *base)
{
    size_t blen = strlen(base);
    return strncmp(entry, base, blen) == 0
        && (entry[blen] == '\0' || entry[blen] == '|');
}

/* ── Root ────────────────────────────────────────────────────────────────── */

static esp_err_t root_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) return send_redirect(req, "/login");
    if (!app_config_is_provisioned()) return send_redirect(req, "/wifi");
    return send_redirect(req, "/status");
}

/* ── Login / logout / change-password ───────────────────────────────────── */

static esp_err_t login_get_handler(httpd_req_t *req)
{
    if (is_authenticated(req)) return send_redirect(req, "/status");
    char query[256] = {0}, err_msg[128] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char raw[128] = {0};
        httpd_query_key_value(query, "error", raw, sizeof(raw));
        url_decode(err_msg, raw, sizeof(err_msg));
    }
    auth_page_open(t(L_LOGIN_TITLE));
    HS("<div class=\"card\">"
       "<div class=\"card-header\"><h6 class=\"mb-0\">"
       "<i class=\"bi bi-lock me-2\"></i>");
    HS(t(L_LOGIN_TITLE));
    HS("</h6></div><div class=\"card-body\">");
    alert_div(err_msg, "danger");
    HS("<form method=\"post\" action=\"/login\">"
       "<div class=\"mb-3\"><label class=\"form-label fw-semibold\">");
    HS(t(L_FIELD_USERNAME));
    HS("</label><input type=\"text\" name=\"username\" class=\"form-control\""
       " autocomplete=\"username\" required autofocus></div>"
       "<div class=\"mb-4\"><label class=\"form-label fw-semibold\">");
    HS(t(L_WIFI_PASSWORD));
    HS("</label><input type=\"password\" name=\"password\" class=\"form-control\""
       " autocomplete=\"current-password\" required></div>"
       "<button type=\"submit\" class=\"btn btn-primary w-100\">"
       "<i class=\"bi bi-box-arrow-in-right me-1\"></i>");
    HS(t(L_LOGIN_BTN));
    HS("</button></form></div></div>");
    auth_page_close();
    return send_html(req);
}

static esp_err_t login_post_handler(httpd_req_t *req)
{
    if (!form_read(req))
        return send_redirect(req, "/login?error=Read+error.");
    char username[64] = {0}, password[64] = {0};
    form_get(s_form_buf, "username", username, sizeof(username));
    form_get(s_form_buf, "password", password, sizeof(password));
    if (strcmp(username, app_config_get_ui_user()) == 0
        && strcmp(password, app_config_get_ui_pass()) == 0) {
        generate_session();
        char cookie[80];
        snprintf(cookie, sizeof(cookie), "session=%s; HttpOnly; Path=/",
                 s_session_token);
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/status");
        httpd_resp_set_hdr(req, "Set-Cookie", cookie);
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, "", 0);
        return ESP_OK;
    }
    return send_redirect(req, "/login?error=Invalid+username+or+password.");
}

static esp_err_t logout_handler(httpd_req_t *req)
{
    clear_session();
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/login");
    httpd_resp_set_hdr(req, "Set-Cookie",
                       "session=; HttpOnly; Path=/; Max-Age=0");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, "", 0);
    return ESP_OK;
}

static esp_err_t change_password_get_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) return send_redirect(req, "/login");
    char query[256] = {0}, err_msg[128] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char raw[128] = {0};
        httpd_query_key_value(query, "error", raw, sizeof(raw));
        url_decode(err_msg, raw, sizeof(err_msg));
    }
    dash_page_open(t(L_NAV_SECURITY), "/change-password");
    HS("<div class=\"card\">"
       "<div class=\"card-header\"><h6 class=\"mb-0\">"
       "<i class=\"bi bi-key me-2\"></i>");
    HS(t(L_SEC_TITLE));
    HS("</h6></div><div class=\"card-body\">");
    if (is_default_creds()) {
        HS("<div class=\"alert alert-warning py-2 small mb-3\">");
        HS(t(L_SEC_DEFAULT_WARN));
        HS("</div>");
    }
    alert_div(err_msg, "danger");
    HS("<form method=\"post\" action=\"/change-password\">"
       "<div class=\"mb-3\"><label class=\"form-label fw-semibold\">");
    HS(t(L_FIELD_USERNAME));
    HF("</label><input type=\"text\" name=\"username\" class=\"form-control\""
       " autocomplete=\"username\" value=\"%s\" required></div>",
       app_config_get_ui_user());
    HS("<div class=\"mb-3\"><label class=\"form-label fw-semibold\">");
    HS(t(L_SEC_NEW_PASS));
    HS("</label><input type=\"password\" name=\"password\" class=\"form-control\""
       " autocomplete=\"new-password\" required></div>"
       "<div class=\"mb-4\"><label class=\"form-label fw-semibold\">");
    HS(t(L_SEC_CONFIRM_PASS));
    HS("</label><input type=\"password\" name=\"password2\" class=\"form-control\""
       " autocomplete=\"new-password\" required></div>"
       "<button type=\"submit\" class=\"btn btn-primary w-100\">"
       "<i class=\"bi bi-check-lg me-1\"></i>");
    HS(t(L_SEC_BTN_SAVE));
    HS("</button></form></div></div>");
    dash_page_close();
    return send_html(req);
}

static esp_err_t change_password_post_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) return send_redirect(req, "/login");
    if (!form_read(req))
        return send_redirect(req, "/change-password?error=Read+error.");
    char username[64] = {0}, password[64] = {0}, password2[64] = {0};
    form_get(s_form_buf, "username",  username,  sizeof(username));
    form_get(s_form_buf, "password",  password,  sizeof(password));
    form_get(s_form_buf, "password2", password2, sizeof(password2));
    if (!username[0])
        return send_redirect(req,
            "/change-password?error=Username+cannot+be+empty.");
    if (strlen(password) < 4)
        return send_redirect(req,
            "/change-password?error=Password+must+be+at+least+4+characters.");
    if (strcmp(password, password2) != 0)
        return send_redirect(req,
            "/change-password?error=Passwords+do+not+match.");
    app_config_save_ui_creds(username, password);
    return send_redirect(req, "/status");
}

/* ── Status dashboard ────────────────────────────────────────────────────── */

static esp_err_t status_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) return send_redirect(req, "/login");
    if (is_default_creds())     return send_redirect(req, "/change-password");

    bool wifi_ok = wifi_manager_is_connected();
    bool mqtt_ok = app_config_get_mqtt_host() && app_config_get_mqtt_host()[0];
    bool zb_ok   = wifi_ok && mqtt_ok;
    const char *ssid = app_config_get_wifi_ssid();
    const char *mqh  = app_config_get_mqtt_host();
    int         mqp  = app_config_get_mqtt_port();
    bool pair_open   = s_pair_open_until_us > esp_timer_get_time();
    int  pair_left   = pair_open
        ? (int)((s_pair_open_until_us - esp_timer_get_time()) / 1000000LL)
        : 0;

    dash_page_open("Dashboard", "/status");
    HS("<div class=\"row g-3 mb-4\">");

    HF("<div class=\"col-12 col-md-4\"><div class=\"card h-100\"><div class=\"card-body\">"
       "<div class=\"d-flex justify-content-between align-items-start\">"
       "<div><div class=\"text-muted small mb-1\"><i class=\"bi bi-wifi me-1\"></i>%s</div>"
       "<span class=\"badge %s status-badge\">%s</span></div>"
       "<i class=\"bi bi-wifi\" style=\"font-size:1.8rem;color:%s\"></i></div>"
       "<div class=\"mt-2 small text-muted\">",
       t(L_STATUS_WIFI),
       wifi_ok ? "bg-success" : "bg-secondary",
       wifi_ok ? t(L_STATUS_CONNECTED) : t(L_STATUS_NOT_CONNECTED),
       wifi_ok ? "#28a745" : "#adb5bd");
    if (wifi_ok && ssid && ssid[0]) HF("SSID: <strong>%s</strong>", ssid);
    else HS(t(L_STATUS_NOT_CONFIGURED));
    HS("</div></div></div></div>");

    HF("<div class=\"col-12 col-md-4\"><div class=\"card h-100\"><div class=\"card-body\">"
       "<div class=\"d-flex justify-content-between align-items-start\">"
       "<div><div class=\"text-muted small mb-1\">"
       "<i class=\"bi bi-hdd-network me-1\"></i>%s</div>"
       "<span class=\"badge %s status-badge\">%s</span></div>"
       "<i class=\"bi bi-hdd-network\" style=\"font-size:1.8rem;color:%s\"></i></div>"
       "<div class=\"mt-2 small text-muted\">",
       t(L_STATUS_MQTT),
       mqtt_ok ? "bg-success" : "bg-secondary",
       mqtt_ok ? t(L_STATUS_CONNECTED) : t(L_STATUS_NOT_CONNECTED),
       mqtt_ok ? "#28a745" : "#adb5bd");
    if (mqtt_ok) HF("<strong>%s</strong>:%d", mqh, mqp ? mqp : 1883);
    else HS(t(L_STATUS_NOT_CONFIGURED));
    HS("</div></div></div></div>");

    HF("<div class=\"col-12 col-md-4\"><div class=\"card h-100\"><div class=\"card-body\">"
       "<div class=\"d-flex justify-content-between align-items-start\">"
       "<div><div class=\"text-muted small mb-1\">"
       "<i class=\"bi bi-diagram-3 me-1\"></i>%s</div>"
       "<span class=\"badge %s status-badge\">%s</span></div>"
       "<i class=\"bi bi-diagram-3\" style=\"font-size:1.8rem;color:%s\"></i></div>"
       "<div class=\"mt-2 small text-muted\">%s</div>"
       "</div></div></div>",
       t(L_STATUS_ZIGBEE),
       zb_ok ? "bg-success" : "bg-secondary",
       zb_ok ? t(L_STATUS_ZB_ACTIVE) : t(L_STATUS_ZB_WAITING),
       zb_ok ? "#28a745" : "#adb5bd",
       zb_ok ? t(L_STATUS_ZB_ACTIVE) : t(L_STATUS_ZB_MSG));

    HS("</div>"); /* row */

    if (pair_open) {
        HS("<div class=\"alert alert-success mb-4\">"
           "<i class=\"bi bi-broadcast me-2\"></i>");
        HS(t(L_STATUS_PAIR_ALERT));
        HS(" &mdash; <strong>");
        HF("%d", pair_left);
        HS("s</strong> ");
        HS(t(L_STATUS_REMAINING));
        HS(". <a href=\"/pair\" class=\"alert-link\">");
        HS(t(L_STATUS_MANAGE_PAIR));
        HS("</a></div>");
    }

    /* Bridged devices list from saved config */
    const char *devs_json = app_config_get_devices();
    cJSON *devs = devs_json ? cJSON_Parse(devs_json) : NULL;
    int dev_count = devs ? cJSON_GetArraySize(devs) : 0;
    HS("<div class=\"card\"><div class=\"card-header\"><h6 class=\"mb-0\">");
    HF("%s (%d)", t(L_STATUS_BRIDGED), dev_count);
    HS("</h6></div>");
    if (dev_count > 0) {
        HS("<ul class=\"list-group list-group-flush rounded-bottom\">");
        cJSON *item;
        cJSON_ArrayForEach(item, devs) {
            const char *raw = cJSON_GetStringValue(item);
            if (!raw) continue;
            char topic[128]; entry_get_topic(raw, topic, sizeof(topic));
            const char *name = strrchr(topic, '/');
            name = name ? name + 1 : topic;
            HF("<li class=\"list-group-item d-flex justify-content-between"
               " align-items-center small\">"
               "<span><strong>%s</strong>"
               " <span class=\"text-muted font-monospace small\">%s</span></span>",
               name, topic);
            HS("<span class=\"badge bg-success\">");
            HS(t(L_STATUS_ACTIVE_BADGE));
            HS("</span></li>");
        }
        HS("</ul>");
    } else {
        HS("<div class=\"card-body text-muted small\">");
        HS(t(L_STATUS_NO_DEVS));
        HS(" <a href=\"/devices\">");
        HS(t(L_STATUS_CFG_DEVS));
        HS(" &rarr;</a></div>");
    }
    HS("</div>");
    if (devs) cJSON_Delete(devs);
    dash_page_close();
    return send_html(req);
}

/* ── WiFi page ───────────────────────────────────────────────────────────── */

static void wifi_scan_task(void *param)
{
    vTaskDelay(pdMS_TO_TICKS(200));
    wifi_scan_config_t cfg = {
        .ssid = NULL, .bssid = NULL, .channel = 0, .show_hidden = true
    };
    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scan start failed: %s", esp_err_to_name(err));
        s_ap_count  = 0;
        s_scan_done = true;
        vTaskDelete(NULL);
        return;
    }
    s_ap_count = 20;
    esp_wifi_scan_get_ap_records(&s_ap_count, s_ap_records);
    s_scan_done = true;
    ESP_LOGI(TAG, "Scan complete, found %d networks", s_ap_count);
    vTaskDelete(NULL);
}

void web_server_trigger_scan(void)
{
    s_scan_done = false;
    xTaskCreate(wifi_scan_task, "wifi_scan", 4096, NULL, 3, NULL);
}

static const char *rssi_icon(int rssi)
{
    if (rssi > -55) return "bi-wifi";
    if (rssi > -70) return "bi-wifi-2";
    return "bi-wifi-1";
}

static esp_err_t wifi_get_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) return send_redirect(req, "/login");
    if (is_default_creds())     return send_redirect(req, "/change-password");

    char query[256] = {0}, err_msg[128] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char raw[128] = {0};
        httpd_query_key_value(query, "error", raw, sizeof(raw));
        url_decode(err_msg, raw, sizeof(err_msg));
    }
    const char *saved_ssid = app_config_get_wifi_ssid();

    dash_page_open(t(L_NAV_WIFI), "/wifi");
    HS("<div class=\"card\">"
       "<div class=\"card-header\"><h6 class=\"mb-0\">"
       "<i class=\"bi bi-wifi me-2\"></i>");
    HS(t(L_WIFI_NETWORK));
    HS("</h6></div><div class=\"card-body\">");
    alert_div(err_msg, "danger");

    /* Scan button — form POSTs to /wifi/scan (PRG); JS intercepts */
    HS("<form method=\"post\" action=\"/wifi/scan\" class=\"mb-3\">"
       "<button id=\"scan-btn\" class=\"btn btn-outline-primary btn-sm\" type=\"submit\">"
       "<i class=\"bi bi-search me-1\"></i>");
    HS(t(L_WIFI_SCAN_BTN));
    HS("</button></form>");

    HS("<form method=\"post\" action=\"/wifi\">");
    if (s_scan_done && s_ap_count > 0) {
        HS("<div id=\"network-list\" class=\"mb-2\">");
        for (int i = 0; i < s_ap_count; i++) {
            const char *auth_str;
            bool is_open = false;
            switch (s_ap_records[i].authmode) {
                case WIFI_AUTH_OPEN:     auth_str = "OPEN"; is_open = true; break;
                case WIFI_AUTH_WPA_PSK:  auth_str = "WPA";  break;
                case WIFI_AUTH_WPA2_PSK: auth_str = "WPA2"; break;
                case WIFI_AUTH_WPA3_PSK: auth_str = "WPA3"; break;
                default:                 auth_str = "WPA2"; break;
            }
            const char *ssid = (const char *)s_ap_records[i].ssid;
            bool chosen  = saved_ssid && strcmp(ssid, saved_ssid) == 0;
            HF("<label class=\"d-flex align-items-center gap-2 wifi-item"
               " p-2 rounded mb-2%s\" data-auth=\"%s\">",
               chosen ? " chosen" : "", auth_str);
            HF("<input type=\"radio\" name=\"ssid\" value=\"%s\""
               " class=\"form-check-input\" data-auth=\"%s\"%s required>",
               ssid, auth_str, chosen ? " checked" : "");
            HF("<i class=\"bi %s\"></i>", rssi_icon(s_ap_records[i].rssi));
            HF("<strong class=\"flex-grow-1\">%s</strong>", ssid);
            HF("<span class=\"badge bg-secondary\">%s</span>", auth_str);
            HF("<small class=\"text-muted\">%d dBm</small></label>",
               s_ap_records[i].rssi);
            (void)is_open;
        }
        HS("</div>");
    } else {
        HS("<div id=\"network-list\" class=\"text-muted small mb-2\">");
        HS(t(L_WIFI_NO_SCAN_YET));
        HS("</div>");
    }

    /* Password section (hidden until network selected) */
    HS("<div id=\"pw-note\" class=\"alert alert-info py-2 small mb-3\" hidden>"
       "<i class=\"bi bi-info-circle me-1\"></i>");
    HS(t(L_WIFI_OPEN_NET));
    HS("</div>"
       "<div id=\"pw-section\" hidden>"
       "<div class=\"mb-3\"><label class=\"form-label fw-semibold\">");
    HS(t(L_WIFI_PASSWORD));
    HS("</label><input type=\"password\" id=\"pw-input\" name=\"password\""
       " class=\"form-control\" placeholder=\"WiFi password\""
       " autocomplete=\"current-password\"></div></div>"
       "<div class=\"d-flex justify-content-end mt-3\">");
    HF("<button id=\"connect-btn\" type=\"submit\" class=\"btn btn-primary\"%s>",
       (s_scan_done && saved_ssid && saved_ssid[0]) ? "" : " disabled");
    HS(t(L_WIFI_CONNECT_BTN));
    HS(" <i class=\"bi bi-arrow-right ms-1\"></i></button>");
    HS("</div></form></div></div>");
    dash_page_close();
    return send_html(req);
}

static esp_err_t wifi_scan_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) return send_redirect(req, "/login");
    web_server_trigger_scan();
    /* Wait for the blocking scan to finish (up to 8 s) before redirecting,
       so the WiFi page immediately shows results. */
    for (int i = 0; i < 80 && !s_scan_done; i++)
        vTaskDelay(pdMS_TO_TICKS(100));
    return send_redirect(req, "/wifi");
}

static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) return send_redirect(req, "/login");
    if (!form_read(req))
        return send_redirect(req, "/wifi?error=Read+error.");
    char ssid[64] = {0}, password[64] = {0};
    form_get(s_form_buf, "ssid",     ssid,     sizeof(ssid));
    form_get(s_form_buf, "password", password, sizeof(password));
    if (!ssid[0])
        return send_redirect(req, "/wifi?error=Please+select+a+network.");

    app_config_save_wifi(ssid, password);
    app_config_set_provisioned(true);
    /* WiFi is already started in APSTA mode — just connect the STA side.
       Do NOT stop/restart WiFi here; the AP must stay up so the user can
       still reach this server and see the connected-IP success page. */
    wifi_manager_connect(ssid, password);
    return send_redirect(req, "/wifi/connecting");
}

static esp_err_t wifi_connecting_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) return send_redirect(req, "/login");
    if (wifi_manager_is_connected())
        return send_redirect(req, "/wifi/connected");

    const char *ssid = app_config_get_wifi_ssid();
    dash_page_open(t(L_WIFI_CONNECTING), "/wifi");
    HS("<div class=\"card\"><div class=\"card-header\"><h6 class=\"mb-0\">"
       "<i class=\"bi bi-wifi me-2\"></i>");
    HS(t(L_WIFI_CONNECTING_TO));
    HF(" %s</h6></div>"
       "<div class=\"card-body text-center py-4\">"
       "<div class=\"spinner-border text-primary mb-3\"></div>"
       "<p class=\"mb-0\">", ssid ? ssid : "network");
    HS(t(L_WIFI_CONNECTING_TO));
    HF(" <strong>%s</strong>&hellip;</p>"
       "<p class=\"text-muted small mt-1\">", ssid ? ssid : "network");
    HS(t(L_WIFI_AUTO_REFRESH));
    HS("</p></div></div>"
       "<meta http-equiv=\"refresh\" content=\"3\">");
    dash_page_close();
    return send_html(req);
}

static esp_err_t wifi_connected_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) return send_redirect(req, "/login");
    const char *ssid = app_config_get_wifi_ssid();
    const char *ip   = wifi_manager_get_ip();
    if (!ip || !ip[0]) ip = "—";
    char next_url[64];
    snprintf(next_url, sizeof(next_url), "http://%s/mqtt", ip);

    dash_page_open(t(L_WIFI_CONNECTED), "/wifi");
    HS("<div class=\"card\">"
       "<div class=\"card-header\" style=\"background:linear-gradient(135deg,#28a745,#20c997)\">"
       "<h6 class=\"mb-0\"><i class=\"bi bi-check-circle me-2\"></i>");
    HS(t(L_WIFI_JOINED));
    HF("</h6></div><div class=\"card-body\">"
       "<p class=\"mb-1\">Successfully joined <strong>%s</strong>.</p>"
       "<p class=\"text-muted small mb-4\">"
       "The gateway has switched to station mode and turned off its access point. "
       "It is now reachable on your local network.</p>"
       "<div class=\"card bg-light border mb-4\"><div class=\"card-body py-3\">"
       "<div class=\"text-muted small mb-1\">",
       ssid ? ssid : "network");
    HS(t(L_WIFI_IP_LABEL));
    HF("</div><div class=\"font-monospace fw-semibold fs-5\">%s</div>"
       "</div></div>"
       "<p class=\"small text-muted mb-3\">"
       "Connect your device to <strong>%s</strong>, then continue setup at the link below.</p>"
       "<a href=\"%s\" class=\"btn btn-primary w-100 mb-2\">"
       "Continue setup at %s <i class=\"bi bi-arrow-right ms-1\"></i></a>"
       "<a href=\"/mqtt\" class=\"btn btn-outline-secondary w-100 btn-sm\">",
       ip, ssid ? ssid : "network", next_url, ip);
    HS(t(L_WIFI_CONTINUE_HERE));
    HS("</a></div></div>");
    dash_page_close();
    return send_html(req);
}

/* ── MQTT page ───────────────────────────────────────────────────────────── */

static esp_err_t mqtt_get_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) return send_redirect(req, "/login");
    if (is_default_creds())     return send_redirect(req, "/change-password");

    char query[256] = {0}, err_msg[128] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char raw[128] = {0};
        httpd_query_key_value(query, "error", raw, sizeof(raw));
        url_decode(err_msg, raw, sizeof(err_msg));
    }
    const char *mqh = app_config_get_mqtt_host();
    int mqp = app_config_get_mqtt_port();
    const char *mqci = app_config_get_mqtt_client_id();
    const char *mqun = app_config_get_mqtt_username();

    dash_page_open(t(L_NAV_MQTT), "/mqtt");
    HS("<div class=\"card\"><div class=\"card-header\"><h6 class=\"mb-0\">"
       "<i class=\"bi bi-hdd-network me-2\"></i>");
    HS(t(L_MQTT_BROKER));
    HS("</h6></div><div class=\"card-body\">");
    alert_div(err_msg, "danger");
    HS("<form method=\"post\" action=\"/mqtt\">"
       "<div class=\"row g-3 mb-3\">"
       "<div class=\"col-8\"><label class=\"form-label fw-semibold\">");
    HS(t(L_MQTT_BROKER_HOST_LBL));
    HF("</label><input type=\"text\" name=\"host\" class=\"form-control\" required value=\"%s\""
       " placeholder=\"192.168.1.x\"></div>", mqh ? mqh : "");
    HS("<div class=\"col-4\"><label class=\"form-label fw-semibold\">");
    HS(t(L_MQTT_PORT));
    HF("</label><input type=\"number\" name=\"port\" class=\"form-control\" value=\"%d\"></div>"
       "</div>", mqp ? mqp : 1883);
    HS("<div class=\"mb-3\"><label class=\"form-label fw-semibold\">");
    HS(t(L_MQTT_CLIENT_ID));
    HF("</label><input type=\"text\" name=\"client_id\" class=\"form-control\" value=\"%s\"></div>",
       (mqci && mqci[0]) ? mqci : "zigbee_gw_01");
    HS("<div class=\"row g-3 mb-4\"><div class=\"col\">"
       "<label class=\"form-label fw-semibold\">");
    HS(t(L_MQTT_USERNAME));
    HS(" <small class=\"fw-normal text-muted\">");
    HS(t(L_MQTT_OPTIONAL));
    HF("</small></label><input type=\"text\" name=\"username\""
       " class=\"form-control\" value=\"%s\"></div>", mqun ? mqun : "");
    HS("<div class=\"col\"><label class=\"form-label fw-semibold\">");
    HS(t(L_MQTT_PASSWORD));
    HS(" <small class=\"fw-normal text-muted\">");
    HS(t(L_MQTT_OPTIONAL));
    HS("</small></label><input type=\"password\" name=\"password\""
       " class=\"form-control\"></div></div>"
       "<div class=\"d-flex justify-content-end\">"
       "<button type=\"submit\" class=\"btn btn-primary\">");
    HS(t(L_BTN_SAVE_CONTINUE));
    HS(" <i class=\"bi bi-arrow-right ms-1\"></i></button>"
       "</div></form></div></div>");
    dash_page_close();
    return send_html(req);
}

static esp_err_t mqtt_post_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) return send_redirect(req, "/login");
    if (!form_read(req))
        return send_redirect(req, "/mqtt?error=Read+error.");
    char host[64] = {0}, client_id[32] = {0};
    char username[32] = {0}, password[64] = {0}, port_str[8] = {0};
    form_get(s_form_buf, "host",      host,      sizeof(host));
    form_get(s_form_buf, "port",      port_str,  sizeof(port_str));
    form_get(s_form_buf, "client_id", client_id, sizeof(client_id));
    form_get(s_form_buf, "username",  username,  sizeof(username));
    form_get(s_form_buf, "password",  password,  sizeof(password));
    if (!host[0])
        return send_redirect(req, "/mqtt?error=Broker+host+is+required.");
    int port = port_str[0] ? atoi(port_str) : 1883;
    if (port <= 0) port = 1883;
    if (!client_id[0]) snprintf(client_id, sizeof(client_id), "zigbee_gw_01");
    app_config_save_mqtt(host, (uint16_t)port, false,
                         client_id, username, password);
    return send_redirect(req, "/devices");
}

/* ── Devices page ────────────────────────────────────────────────────────── */

/* Flush the HTML build buffer as an HTTP chunk and reset it. */
static void flush_chunk(httpd_req_t *req)
{
    if (s_html_len > 0) {
        httpd_resp_send_chunk(req, s_html, s_html_len);
        html_reset();
    }
}

static const char *wb_drv_type(const wb_control_t *ctrl)
{
    if (ctrl->type_id == WB_TYPE_TEXT)
        return "wb_text";
    if (ctrl->type_id == WB_TYPE_RANGE && wb_control_is_writable(ctrl))
        return "wb_range";
    return wb_control_is_writable(ctrl) ? "wb_switch" : "wb_sensor";
}

/* The source the Devices page works with. A saved choice wins while it is possible;
   without WB topics only Zigbee2MQTT is available, and with WB topics and no choice
   yet plain Wirenboard is the default. */
static uint8_t effective_disc_mode(void)
{
    uint8_t saved = app_config_get_discovery_mode();
    if (!wb_driver_wb_present()) return DISC_MODE_Z2M;
    return saved == DISC_MODE_AUTO ? DISC_MODE_WB : saved;
}

/* Escape text for use in HTML element content and double-quoted attributes. */
static void html_esc(char *out, size_t sz, const char *in)
{
    size_t n = 0;
    for (; *in && n + 7 < sz; in++) {
        const char *r = NULL;
        switch (*in) {
        case '&': r = "&amp;";  break;
        case '<': r = "&lt;";   break;
        case '>': r = "&gt;";   break;
        case '"': r = "&quot;"; break;
        default: break;
        }
        if (r) { size_t l = strlen(r); memcpy(out + n, r, l); n += l; }
        else out[n++] = *in;
    }
    out[n] = '\0';
}

/* Zigbee2MQTT mode: devices from bridge/devices, grouped like the WB controls.
   Returns true when at least one property was listed. */
static bool render_z2m_list(httpd_req_t *req, cJSON *saved)
{
    const z2m_entry_t *e = NULL;
    int n = z2m_driver_get_entries(&e);

    if (n == 0) {
        HS("<div class=\"card mb-2\"><div class=\"card-body text-center py-4\">"
           "<i class=\"bi bi-cpu\" style=\"font-size:2rem;color:#667eea\"></i>"
           "<p class=\"mt-2 text-muted mb-1\">");
        HS(z2m_driver_ready() ? t(L_DEVICES_Z2M_NONE) : t(L_DEVICES_NO_CTRLS));
        HS("</p><a href=\"/devices/scan\" class=\"btn btn-sm btn-outline-secondary\">"
           "<i class=\"bi bi-arrow-clockwise\"></i> ");
        HS(t(L_DEVICES_RELOAD));
        HS("</a></div></div>");
        return false;
    }

    HS("<script>"
       "function z2Sel(cb,id){"
       "document.querySelectorAll('.z2c-'+id).forEach(c=>c.checked=cb.checked);}"
       "function z2Chk(id){"
       "var cs=document.querySelectorAll('.z2c-'+id);"
       "var hdr=document.getElementById('z2h-'+id);"
       "var n=0;cs.forEach(c=>{if(c.checked)n++;});"
       "hdr.checked=n===cs.length;hdr.indeterminate=n>0&&n<cs.length;}"
       "</script>");

    int group = 0;
    for (int i = 0; i < n; ) {
        int j = i;
        while (j < n && strcmp(e[j].device, e[i].device) == 0) j++;
        int cnt = j - i;

        char esc_dev[Z2M_NAME_LEN * 6 + 8];
        html_esc(esc_dev, sizeof(esc_dev), e[i].device);

        /* How many properties of this device are already saved? */
        int saved_count = 0;
        for (int k = i; k < j; k++) {
            char base[128];
            snprintf(base, sizeof(base), "z2m|%s/%s#%s", Z2M_BASE_TOPIC, e[k].device, e[k].prop);
            if (saved && cJSON_IsArray(saved)) {
                cJSON *it;
                cJSON_ArrayForEach(it, saved) {
                    if (cJSON_IsString(it) && entry_matches_base(it->valuestring, base))
                        { saved_count++; break; }
                }
            }
        }

        HS("<div class=\"card mb-2\"><div class=\"card-header py-2 d-flex align-items-center gap-2\">");
        HF("<input type=\"checkbox\" id=\"z2h-%d\""
           " class=\"form-check-input flex-shrink-0\" style=\"width:1.3em;height:1.3em\""
           " onchange=\"z2Sel(this,%d)\"%s>",
           group, group, saved_count == cnt ? " checked" : "");
        HF("<i class=\"bi bi-cpu\"></i><strong>%s</strong>", esc_dev);
        if (e[i].model[0]) {
            char esc_model[24 * 6 + 8], esc_vendor[16 * 6 + 8];
            html_esc(esc_model, sizeof(esc_model), e[i].model);
            html_esc(esc_vendor, sizeof(esc_vendor), e[i].vendor);
            HF("<small class=\"text-muted opacity-75\">%s %s</small>", esc_vendor, esc_model);
        }
        HF("<span class=\"badge bg-secondary ms-auto\">%d ", cnt);
        HS(t(L_DEVICES_CONTROLS));
        HS("</span></div><div class=\"card-body p-0\"><table class=\"table table-sm mb-0\"><tbody>");

        for (int k = i; k < j; k++) {
            char val[200], esc_val[400];
            snprintf(val, sizeof(val), "z2m|%s/%s#%s|%s|%s|%s",
                     Z2M_BASE_TOPIC, e[k].device, e[k].prop,
                     e[k].vendor[0] ? e[k].vendor : "Z2M",
                     e[k].model[0]  ? e[k].model  : e[k].prop,
                     z2m_kind_name(e[k].kind));
            html_esc(esc_val, sizeof(esc_val), val);

            char base[128];
            snprintf(base, sizeof(base), "z2m|%s/%s#%s", Z2M_BASE_TOPIC, e[k].device, e[k].prop);
            bool checked = false;
            if (saved && cJSON_IsArray(saved)) {
                cJSON *it;
                cJSON_ArrayForEach(it, saved) {
                    if (cJSON_IsString(it) && entry_matches_base(it->valuestring, base))
                        { checked = true; break; }
                }
            }
            HF("<tr><td class=\"ps-4\" style=\"width:2.5em\">"
               "<input type=\"checkbox\" name=\"topic\" value=\"%s\""
               " class=\"form-check-input z2c-%d\" style=\"width:1.2em;height:1.2em\""
               " onchange=\"z2Chk(%d)\"%s></td>",
               esc_val, group, group, checked ? " checked" : "");
            HF("<td><strong>%s</strong></td>"
               "<td><span class=\"badge bg-light text-dark border\">%s</span>%s</td></tr>",
               e[k].prop, z2m_kind_name(e[k].kind),
               e[k].writable
                   ? " <i class=\"bi bi-pencil text-primary\" title=\"writable\"></i>"
                   : " <i class=\"bi bi-eye text-muted\" title=\"read-only\"></i>");
        }
        HS("</tbody></table></div></div>");
        flush_chunk(req);
        group++;
        i = j;
    }

    if (z2m_driver_truncated())
        HF("<div class=\"alert alert-warning small\">%d+ properties &mdash; list truncated.</div>",
           Z2M_MAX_ENTRIES);
    return true;
}

static esp_err_t devices_get_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) return send_redirect(req, "/login");
    if (is_default_creds())     return send_redirect(req, "/change-password");

    char query[256] = {0}, err_msg[128] = {0};
    bool scanning = false;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char raw[128] = {0};
        httpd_query_key_value(query, "error", raw, sizeof(raw));
        url_decode(err_msg, raw, sizeof(err_msg));
        char raw3[8] = {0};
        if (httpd_query_key_value(query, "scanning", raw3, sizeof(raw3)) == ESP_OK)
            scanning = (raw3[0] == '1');
    }

    httpd_resp_set_type(req, "text/html");

    dash_page_open(t(L_DEVICES_PAGE_TITLE), "/devices");
    alert_div(err_msg, "danger");

    bool mqtt_up = mqtt_client_is_connected();
    if (!mqtt_up) {
        HS("<div class=\"alert alert-warning d-flex align-items-center gap-2\" role=\"alert\">"
           "<i class=\"bi bi-exclamation-triangle-fill\"></i>"
           "<span><strong>");
        HS(t(L_DEVICES_MQTT_WARN));
        HS("</strong> &mdash; ");
        HS(t(L_DEVICES_MQTT_CFG));
        HS("</span></div>");
    }

    if (scanning) {
        HS("<div class=\"alert alert-info d-flex align-items-center gap-2\" role=\"alert\">"
           "<div class=\"spinner-border spinner-border-sm\" role=\"status\"></div>"
           "<span>");
        HS(t(L_DEVICES_SCANNING));
        HS("</span></div>"
           "<meta http-equiv=\"refresh\" content=\"5;url=/devices\">");
    }

    HS("<div class=\"card mb-3\"><div class=\"card-body\">"
       "<form method=\"get\" action=\"/devices/scan\""
       " class=\"d-flex gap-2 align-items-center\">"
       "<button type=\"submit\" class=\"btn btn-sm btn-outline-secondary\">"
       "<i class=\"bi bi-arrow-clockwise\"></i> ");
    HS(t(L_DEVICES_SCAN_BTN));
    HS("</button>");
    {
        bool wb_ok = wb_driver_wb_present();
        uint8_t mode = effective_disc_mode();
        static const struct { uint8_t mode; locale_key_t label; } opts[] = {
            { DISC_MODE_WB,     L_DEVICES_SRC_WB     },
            { DISC_MODE_WB_ALL, L_DEVICES_SRC_WB_ALL },
            { DISC_MODE_Z2M,    L_DEVICES_SRC_Z2M    },
        };
        for (size_t o = 0; o < sizeof(opts) / sizeof(opts[0]); o++) {
            bool avail = (opts[o].mode == DISC_MODE_Z2M) || wb_ok;
            HF("<div class=\"form-check ms-3 mb-0\">"
               "<input class=\"form-check-input\" type=\"radio\" name=\"mode\" id=\"src-%d\" value=\"%d\""
               " onchange=\"this.form.submit()\"%s%s>"
               "<label class=\"form-check-label small\" for=\"src-%d\">",
               opts[o].mode, opts[o].mode,
               mode == opts[o].mode ? " checked" : "",
               avail ? "" : " disabled",
               opts[o].mode);
            HS(t(opts[o].label));
            HS("</label></div>");
        }
    }
    HS("</form></div></div>");
    flush_chunk(req);

    const char *saved_json_str = app_config_get_devices();
    cJSON *saved = saved_json_str ? cJSON_Parse(saved_json_str) : NULL;

    HS("<form method=\"post\" action=\"/devices\">");

    /* Non-WB MQTT / zigbee2mqtt scan disabled — WB only for now.
    {
        bool has_capture = mqtt_client_is_capture_ready();
        const char *bridge_json = has_capture ? mqtt_client_get_capture_buf() : NULL;
        cJSON *dev_arr = bridge_json ? cJSON_Parse(bridge_json) : NULL;
        // ... zigbee2mqtt rich view and generic MQTT topic fallback ...
        if (dev_arr) cJSON_Delete(dev_arr);
    }
    */

    bool z2m_mode = (effective_disc_mode() == DISC_MODE_Z2M);
    bool z2m_listed = z2m_mode && render_z2m_list(req, saved);

    /* Wirenboard controls — grouped by device */
    int wb_dev_count = z2m_mode ? 0 : wb_driver_get_device_count();
    if (wb_dev_count > 0 && !wb_driver_has_controls()) {
        /* Device list known but control table freed after discovery window. */
        HS("<div class=\"card mb-2\"><div class=\"card-body text-center py-4\">"
           "<i class=\"bi bi-cpu\" style=\"font-size:2rem;color:#667eea\"></i>"
           "<p class=\"mt-2 text-muted mb-1\">");
        HS(t(L_DEVICES_NO_CTRLS));
        HS("</p><a href=\"/devices/scan\" class=\"btn btn-sm btn-outline-secondary\">"
           "<i class=\"bi bi-arrow-clockwise\"></i> ");
        HS(t(L_DEVICES_RELOAD));
        HS("</a></div></div>");
    } else if (wb_dev_count > 0) {
        /* Inline JS: device header checkbox toggles all its children */
        HS("<script>"
           "function wbSel(cb,id){"
           "document.querySelectorAll('.wbc-'+id).forEach(c=>c.checked=cb.checked);}"
           "function wbChk(id){"
           "var cs=document.querySelectorAll('.wbc-'+id);"
           "var hdr=document.getElementById('wbh-'+id);"
           "var n=0;cs.forEach(c=>{if(c.checked)n++;});"
           "hdr.checked=n===cs.length;hdr.indeterminate=n>0&&n<cs.length;}"
           "</script>");

        for (int d = 0; d < wb_dev_count; d++) {
            const wb_device_t *dev = wb_driver_get_device(d);
            if (!dev) continue;

            static const wb_control_t *ctrls[WB_MAX_CONTROLS];
            int ctrl_count = wb_driver_get_controls(dev->id, ctrls, WB_MAX_CONTROLS);
            if (ctrl_count == 0) continue;

            /* Count how many controls of this device are already saved */
            int saved_count = 0;
            for (int c = 0; c < ctrl_count; c++) {
                const wb_control_t *ctrl = ctrls[c];
                const char *drv_type = wb_drv_type(ctrl);
                char cbval[200];
                snprintf(cbval, sizeof(cbval), "%s|/devices/%s/controls/%s",
                         drv_type, dev->id, ctrl->name);
                if (saved && cJSON_IsArray(saved)) {
                    cJSON *it;
                    cJSON_ArrayForEach(it, saved) {
                        if (cJSON_IsString(it) && entry_matches_base(it->valuestring, cbval))
                            { saved_count++; break; }
                    }
                }
            }
            bool all_saved  = (saved_count == ctrl_count && ctrl_count > 0);
            bool some_saved = (saved_count > 0 && saved_count < ctrl_count);

            /* Device header card */
            HS("<div class=\"card mb-2\">");

            /* Header row: device checkbox + name */
            HS("<div class=\"card-header py-2 d-flex align-items-center gap-2\">");
            HF("<input type=\"checkbox\" id=\"wbh-%s\""
               " class=\"form-check-input flex-shrink-0\" style=\"width:1.3em;height:1.3em\""
               " onchange=\"wbSel(this,'%s')\"%s%s>",
               dev->id, dev->id,
               all_saved  ? " checked" : "",
               some_saved ? " data-indeterminate=\"1\"" : "");
            HF("<i class=\"bi bi-cpu\"></i><strong>%s</strong>", dev->name);
            if (strcmp(dev->name, dev->id) != 0)
                HF("<small class=\"text-muted opacity-75\">%s</small>", dev->id);
            HF("<span class=\"badge bg-secondary ms-auto\">%d ", ctrl_count);
            HS(t(L_DEVICES_CONTROLS));
            HS("</span>");
            HS("</div>");

            /* Controls table */
            HS("<div class=\"card-body p-0\">"
               "<table class=\"table table-sm mb-0\"><tbody>");

            for (int c = 0; c < ctrl_count; c++) {
                const wb_control_t *ctrl = ctrls[c];
                const char *drv_type = wb_drv_type(ctrl);

                char ctrl_topic[160];
                snprintf(ctrl_topic, sizeof(ctrl_topic),
                         "/devices/%s/controls/%s", dev->id, ctrl->name);
                char checkbox_val[200];
                snprintf(checkbox_val, sizeof(checkbox_val), "%s|%s", drv_type, ctrl_topic);

                bool checked = false;
                if (saved && cJSON_IsArray(saved)) {
                    cJSON *it;
                    cJSON_ArrayForEach(it, saved) {
                        if (cJSON_IsString(it) && entry_matches_base(it->valuestring, checkbox_val))
                            { checked = true; break; }
                    }
                }

                HF("<tr title=\"%s\">", ctrl_topic);
                HF("<td class=\"ps-4\" style=\"width:2.5em\">"
                   "<input type=\"checkbox\" name=\"topic\" value=\"%s\""
                   " class=\"form-check-input wbc-%s\" style=\"width:1.2em;height:1.2em\""
                   " onchange=\"wbChk('%s')\"%s>"
                   "</td>",
                   checkbox_val, dev->id, dev->id, checked ? " checked" : "");
                HF("<td><strong>%s</strong></td>", ctrl->name);
                HF("<td>"
                   "<span class=\"badge bg-light text-dark border\">%s</span>"
                   " <small class=\"text-muted\">%s</small>"
                   "%s"
                   "</td>",
                   wb_control_type_name(ctrl),
                   drv_type,
                   (ctrl->flags & WB_FLAG_READONLY)
                       ? " <i class=\"bi bi-eye text-muted\" title=\"read-only\"></i>"
                       : " <i class=\"bi bi-pencil text-primary\" title=\"writable\"></i>");
                HF("<td class=\"text-end pe-3\"><small class=\"text-muted\">%s</small></td>",
                   ctrl->units);
                HS("</tr>");
                if ((c & 7) == 7) flush_chunk(req); /* flush every 8 rows */
            }

            HS("</tbody></table></div></div>"); /* table, card-body, card */

            /* Set indeterminate state via script after render */
            if (some_saved)
                HF("<script>document.getElementById('wbh-%s').indeterminate=true;</script>",
                   dev->id);

            flush_chunk(req);
        }
    }

    /* When WB scan hasn't run yet, show the already-saved devices so the user
       can see what is configured without needing a live MQTT connection. */
    if (wb_dev_count == 0 && !z2m_listed && saved && cJSON_IsArray(saved) && cJSON_GetArraySize(saved) > 0) {
        HS("<div class=\"card mb-3\"><div class=\"card-header py-2\">"
           "<h6 class=\"mb-0\"><i class=\"bi bi-hdd-stack me-2\"></i>");
        HS(t(L_DEVICES_PAGE_TITLE));
        HS("</h6></div>"
           "<div class=\"card-body p-0\">"
           "<table class=\"table table-sm mb-0\"><thead><tr>"
           "<th class=\"ps-3\">");
        HS(t(L_DEVICES_TOPIC));
        HS("</th><th>");
        HS(t(L_DEVICES_TYPE));
        HS("</th></tr></thead><tbody>");
        cJSON *it;
        cJSON_ArrayForEach(it, saved) {
            const char *raw = cJSON_IsString(it) ? it->valuestring : NULL;
            if (!raw) continue;
            char topic[128], type_buf[32];
            entry_get_topic(raw, topic, sizeof(topic));
            entry_get_type(raw, type_buf, sizeof(type_buf));
            HF("<tr><td class=\"ps-3 font-monospace small\">%s</td>"
               "<td><span class=\"badge bg-secondary\">%s</span></td></tr>",
               topic, type_buf);
        }
        HS("</tbody></table></div></div>");
        flush_chunk(req);
    }

    HS("<div class=\"d-flex justify-content-end mt-3\">"
       "<button type=\"submit\" class=\"btn btn-primary\">");
    HS(t(L_BTN_SAVE_CONTINUE));
    HS(" <i class=\"bi bi-arrow-right ms-1\"></i></button>"
       "</div></form>");

    if (saved) cJSON_Delete(saved);

    dash_page_close();
    flush_chunk(req);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* Subscribe to <prefix>/bridge/devices, wait for retained payload, then redirect. */
/* Unified scan: triggers WB discovery, MQTT topic rescan, and bridge/devices
   capture all at once, then redirects immediately — no blocking delay. The
   /devices page detects ?scanning=1 and auto-refreshes after 5 s. */
static esp_err_t devices_scan_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) return send_redirect(req, "/login");

    char query[64] = {0}, tmp[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "mode", tmp, sizeof(tmp)) == ESP_OK) {
        int mode = atoi(tmp);
        if (mode >= DISC_MODE_WB && mode <= DISC_MODE_Z2M &&
            (mode == DISC_MODE_Z2M || wb_driver_wb_present())) {
            app_config_set_discovery_mode((uint8_t)mode);
            wb_driver_set_mode((uint8_t)mode);
        }
    }

    if (mqtt_client_is_connected()) {
        wb_driver_refresh();
        if (effective_disc_mode() == DISC_MODE_Z2M) z2m_driver_refresh();
        else                                         z2m_driver_release();
    }

    return send_redirect(req, "/devices?scanning=1");
}

static esp_err_t devices_post_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) return send_redirect(req, "/login");
    if (!form_read(req))
        return send_redirect(req, "/devices?error=Read+error.");

    /* Collect all selected topics into a JSON array */
    cJSON *arr = cJSON_CreateArray();
    const char *p = s_form_buf;
    while (p && *p) {
        const char *key_start = p;
        const char *eq = strchr(p, '=');
        if (!eq) break;
        size_t key_len = eq - key_start;
        if (key_len == 5 && strncmp(key_start, "topic", 5) == 0) {
            char val[256] = {0};
            p = eq + 1;
            const char *amp = strchr(p, '&');
            size_t vlen = amp ? (size_t)(amp - p) : strlen(p);
            if (vlen < sizeof(val)) {
                memcpy(val, p, vlen);
                val[vlen] = '\0';
                char decoded[256] = {0};
                url_decode(decoded, val, sizeof(decoded));
                cJSON_AddItemToArray(arr, cJSON_CreateString(decoded));
            }
            p = amp ? amp + 1 : NULL;
        } else {
            p = strchr(p, '&');
            if (p) p++;
        }
    }

    if (cJSON_GetArraySize(arr) == 0) {
        cJSON_Delete(arr);
        return send_redirect(req, "/devices?error=Select+at+least+one+device.");
    }

    char *json_str = cJSON_PrintUnformatted(arr);
    if (json_str) {
        app_config_save_devices(json_str, strlen(json_str));
        free(json_str);
        s_needs_restart = true;
    }
    cJSON_Delete(arr);
    return send_redirect(req, "/pair");
}

/* ── Devices / configure page ────────────────────────────────────────────── */

/* Derive default model string from a saved entry string. */
static void entry_default_model(const char *entry, char *out, size_t out_sz)
{
    char topic[128];
    entry_get_topic(entry, topic, sizeof(topic));
    /* /devices/<dev>/controls/<ctrl> → <dev>/<ctrl> */
    const char *d = strstr(topic, "/devices/");
    if (d) d += 9; else { d = topic; }
    const char *c = strstr(d, "/controls/");
    if (c)
        snprintf(out, out_sz, "%.*s/%s", (int)(c - d), d, c + 10);
    else
        snprintf(out, out_sz, "%s", d);
}

/* Extract mfr from a 4-part entry string (or return default "WB"). */
static void entry_get_mfr(const char *entry, char *out, size_t out_sz)
{
    /* skip type|topic| */
    const char *p = strchr(entry, '|');
    if (!p) { strncpy(out, "WB", out_sz-1); out[out_sz-1]='\0'; return; }
    p = strchr(p+1, '|');
    if (!p) { strncpy(out, "WB", out_sz-1); out[out_sz-1]='\0'; return; }
    p++;
    const char *end = strchr(p, '|');
    size_t len = end ? (size_t)(end-p) : strlen(p);
    if (len == 0) { strncpy(out, "WB", out_sz-1); out[out_sz-1]='\0'; return; }
    if (len >= out_sz) len = out_sz-1;
    memcpy(out, p, len); out[len]='\0';
}

/* Extract model from entry (4th pipe field); stops before optional 5th field. */
static void entry_get_model(const char *entry, char *out, size_t out_sz)
{
    const char *p = strchr(entry, '|');
    if (!p) { entry_default_model(entry, out, out_sz); return; }
    p = strchr(p+1, '|');
    if (!p) { entry_default_model(entry, out, out_sz); return; }
    p = strchr(p+1, '|');
    if (!p) { entry_default_model(entry, out, out_sz); return; }
    p++;
    if (!p[0]) { entry_default_model(entry, out, out_sz); return; }
    const char *end = strchr(p, '|');   /* stop before 5th field if present */
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, p, len); out[len] = '\0';
    if (!len) entry_default_model(entry, out, out_sz);
}

/* Extract Zigbee subtype from optional 5th pipe field ("light" or "switch"). */
static void entry_get_subtype(const char *entry, char *out, size_t out_sz)
{
    const char *p = entry;
    for (int i = 0; i < 4; i++) {
        p = strchr(p, '|');
        if (!p) { out[0] = '\0'; return; }
        p++;
    }
    const char *end = strchr(p, '|');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, p, len); out[len] = '\0';
}

static esp_err_t configure_get_handler(httpd_req_t *req)
{
    /* Identity editing moved to the Pair page. */
    return send_redirect(req, "/pair");
}

static esp_err_t configure_post_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) return send_redirect(req, "/login");
    if (!form_read(req))
        return send_redirect(req, "/devices/configure?error=Read+error.");

    /* Parse sequential (entry, mfr, model, subtype) quads from the body.
       HTML forms preserve field DOM order, so quads arrive in-order. */
    cJSON *arr = cJSON_CreateArray();
    const char *p = s_form_buf;
    int phase = 0; /* 0=entry, 1=mfr, 2=model, 3=subtype */
    char cur_entry[160] = {0}, cur_mfr[33] = {0}, cur_model[33] = {0};

    while (p && *p) {
        const char *eq  = strchr(p, '=');
        if (!eq) break;
        size_t klen = (size_t)(eq - p);
        char key[16] = {0};
        if (klen < sizeof(key)) memcpy(key, p, klen);
        p = eq + 1;
        const char *amp = strchr(p, '&');
        size_t vlen = amp ? (size_t)(amp - p) : strlen(p);
        char raw[200] = {0};
        if (vlen < sizeof(raw)) memcpy(raw, p, vlen);
        p = amp ? amp + 1 : NULL;

        char decoded[200] = {0};
        url_decode(decoded, raw, sizeof(decoded));

        if (strcmp(key, "entry") == 0) {
            strncpy(cur_entry, decoded, sizeof(cur_entry)-1);
            cur_mfr[0] = '\0'; cur_model[0] = '\0';
            phase = 1;
        } else if (strcmp(key, "mfr") == 0 && phase == 1) {
            strncpy(cur_mfr, decoded, sizeof(cur_mfr)-1);
            phase = 2;
        } else if (strcmp(key, "model") == 0 && phase == 2) {
            strncpy(cur_model, decoded, sizeof(cur_model)-1);
            phase = 3;
        } else if (strcmp(key, "subtype") == 0 && phase == 3) {
            /* "type|topic|mfr|model[|subtype]" — only append subtype when non-empty */
            char sub16[16] = {0};
            strncpy(sub16, decoded, 15);  /* "light" or "switch" — short by design */
            char full[260];
            if (sub16[0])
                snprintf(full, sizeof(full), "%s|%s|%s|%s", cur_entry, cur_mfr, cur_model, sub16);
            else
                snprintf(full, sizeof(full), "%s|%s|%s", cur_entry, cur_mfr, cur_model);
            cJSON_AddItemToArray(arr, cJSON_CreateString(full));
            phase = 0;
        }
    }

    char *json_str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (json_str) {
        app_config_save_devices(json_str, strlen(json_str));
        free(json_str);
        s_needs_restart = true;
    }
    return send_redirect(req, "/pair");
}

/* ── Pair page ───────────────────────────────────────────────────────────── */

static esp_err_t pair_get_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) return send_redirect(req, "/login");
    if (is_default_creds())     return send_redirect(req, "/change-password");

    int64_t now    = esp_timer_get_time();
    bool    open_  = s_pair_open_until_us > now;
    int     left   = open_ ? (int)((s_pair_open_until_us - now) / 1000000LL) : 0;
    bool    paired = s_paired_ieee[0] != '\0';

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char gw_ieee[32];
    snprintf(gw_ieee, sizeof(gw_ieee),
             "%02x:%02x:%02x:ff:fe:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    char pan_str[8], ch_str[4], addr_str[8];
    if (s_zb_pan_id)     snprintf(pan_str,  sizeof(pan_str),  "0x%04X", s_zb_pan_id);
    else                 snprintf(pan_str,  sizeof(pan_str),  "\xe2\x80\x94");
    if (s_zb_channel)    snprintf(ch_str,   sizeof(ch_str),   "%u", s_zb_channel);
    else                 snprintf(ch_str,   sizeof(ch_str),   "\xe2\x80\x94");
    if (s_zb_short_addr) snprintf(addr_str, sizeof(addr_str), "0x%04X", s_zb_short_addr);
    else                 snprintf(addr_str, sizeof(addr_str), "\xe2\x80\x94");
    const char *ieee_display = s_zb_ieee[0] ? s_zb_ieee : gw_ieee;

    httpd_resp_set_type(req, "text/html");
    dash_page_open(t(L_NAV_PAIR), "/pair");

    HS("<div class=\"card mb-3\"><div class=\"card-header\"><h6 class=\"mb-0\">"
       "<i class=\"bi bi-broadcast me-2\"></i>");
    HS(t(L_PAIR_CARD_TITLE));
    HS("</h6></div><div class=\"card-body\">");

    if (paired) {
        HS("<div class=\"card mb-4 border-success\">"
           "<div class=\"card-header py-2\" style=\"background:#d1e7dd;color:#0a3622\">"
           "<h6 class=\"mb-0\">"
           "<i class=\"bi bi-check-circle-fill me-2\"></i>");
        HS(t(L_PAIR_WITH_REMOTE));
        HS("</h6></div><div class=\"card-body p-0\">"
           "<table class=\"table table-sm mb-0\"><tbody>");
        HF("<tr><th class=\"ps-3 text-muted fw-normal\" style=\"width:40%%\">%s</th>"
           "<td class=\"fw-semibold\">%s</td></tr>", t(L_PAIR_NAME), s_paired_name);
        HF("<tr><th class=\"ps-3 text-muted fw-normal\">%s</th>"
           "<td class=\"font-monospace\">%s</td></tr>", t(L_PAIR_IEEE), s_paired_ieee);
        HF("<tr><th class=\"ps-3 text-muted fw-normal\">%s</th>"
           "<td>%s</td></tr>", t(L_PAIR_MODEL), s_paired_model);
        HS("</tbody></table></div></div>"
           "<form method=\"post\" action=\"/pair/unpair\" class=\"mb-4\">"
           "<button class=\"btn btn-outline-danger\">"
           "<i class=\"bi bi-x-circle me-1\"></i>");
        HS(t(L_PAIR_UNPAIR));
        HS("</button></form>");
    } else if (open_) {
        HS("<p class=\"text-muted small mb-3\">");
        HS(t(L_PAIR_DESC));
        HS("</p>");
        HF("<form method=\"post\" action=\"/pair/stop\" class=\"mb-4\">"
           "<button class=\"btn btn-danger\">"
           "<i class=\"bi bi-broadcast me-1\"></i>%s "
           "<span id=\"pair-countdown\" data-seconds=\"%d\">",
           t(L_PAIR_STOP_BTN), left);
        HS(t(L_PAIR_WINDOW_OPEN));
        HF(" \xe2\x80\x94 %ds %s\xe2\x80\xa6</span>"
           "</button></form>"
           "<meta http-equiv=\"refresh\" content=\"5\">",
           left, t(L_PAIR_REMAINING));
    } else if (s_zb_pan_id) {
        HS("<p class=\"text-muted small mb-3\">");
        HS(t(L_PAIR_DESC));
        HS("</p><form method=\"post\" action=\"/pair/leave\" class=\"mb-4\">"
           "<button class=\"btn btn-outline-danger\">"
           "<i class=\"bi bi-x-octagon me-1\"></i>");
        HS(t(L_PAIR_LEAVE));
        HS("</button></form>");
    } else {
        HS("<p class=\"text-muted small mb-3\">");
        HS(t(L_PAIR_DESC));
        HS("</p><form method=\"post\" action=\"/pair/open\" class=\"mb-4\">"
           "<button class=\"btn btn-success\">"
           "<i class=\"bi bi-broadcast me-1\"></i>");
        HS(t(L_PAIR_START));
        HS("</button></form>");
    }

    HS("<div class=\"card mb-3\"><div class=\"card-header py-2\">"
       "<h6 class=\"mb-0\"><i class=\"bi bi-cpu me-2\"></i>");
    HS(t(L_PAIR_GW_IDENTITY));
    HF("</h6></div><div class=\"card-body p-0\">"
       "<table class=\"table table-sm mb-0\"><tbody>"
       "<tr><th class=\"ps-3 text-muted fw-normal\" style=\"width:40%%\">%s</th>"
       "<td class=\"fw-semibold\">ZigbeeMQTT Gateway</td></tr>"
       "<tr><th class=\"ps-3 text-muted fw-normal\">%s</th>"
       "<td class=\"font-monospace small\">%s</td></tr>"
       "<tr><th class=\"ps-3 text-muted fw-normal\">%s</th>"
       "<td class=\"font-monospace\">%s</td></tr>"
       "<tr><th class=\"ps-3 text-muted fw-normal\">%s</th>"
       "<td>%s</td></tr>"
       "<tr><th class=\"ps-3 text-muted fw-normal\">%s</th>"
       "<td class=\"font-monospace\">%s</td></tr>"
       "</tbody></table></div></div>",
       t(L_PAIR_DEV_NAME), t(L_PAIR_IEEE), ieee_display,
       t(L_PAIR_PAN_ID), pan_str,
       t(L_STATUS_CHANNEL), ch_str,
       t(L_STATUS_ADDR), addr_str);

    HS("</div></div>"); /* card-body, card */
    flush_chunk(req);

    /* ── Endpoint Zigbee Identity — editable table ── */
    const char *devs_json = app_config_get_devices();
    cJSON *devs = devs_json ? cJSON_Parse(devs_json) : NULL;
    int dev_count = devs ? cJSON_GetArraySize(devs) : 0;

    if (dev_count > 0) {
        HS("<div class=\"card mb-3\">"
           "<div class=\"card-header py-2\">"
           "<h6 class=\"mb-0\"><i class=\"bi bi-diagram-3 me-2\"></i>");
        HS(t(L_PAIR_ZB_IDENTITY));
        HS("</h6></div>"
           "<form method=\"post\" action=\"/devices/configure\">"
           "<div class=\"card-body p-0\">"
           "<table class=\"table table-sm mb-0\">"
           "<thead class=\"table-light\">"
           "<tr>");
        HF("<th class=\"ps-3\" style=\"width:46px\">%s</th>"
           "<th>%s</th><th style=\"width:110px\">%s</th>"
           "<th style=\"width:145px\">%s</th>"
           "<th style=\"width:115px\">Appearance</th>",
           t(L_PAIR_EP_COL), t(L_DEVICES_TOPIC),
           t(L_PAIR_MANUFACTURER), t(L_PAIR_MODEL));
        HS("</tr></thead><tbody>");
        int ep = 1;
        cJSON *item;
        cJSON_ArrayForEach(item, devs) {
            const char *raw = cJSON_GetStringValue(item);
            if (!raw) continue;
            char topic[128]; entry_get_topic(raw, topic, sizeof(topic));
            const char *name = strrchr(topic, '/');
            name = name ? name + 1 : topic;
            char mfr[33], model[65], subtype[16];
            entry_get_mfr(raw, mfr, sizeof(mfr));
            entry_get_model(raw, model, sizeof(model));
            entry_get_subtype(raw, subtype, sizeof(subtype));
            char type_buf[32]; entry_get_type(raw, type_buf, sizeof(type_buf));
            bool is_onoff = (strcmp(type_buf, "wb_switch") == 0);
            char base_entry[200];
            snprintf(base_entry, sizeof(base_entry), "%s|%s", type_buf, topic);
            HF("<tr>"
               "<td class=\"ps-3 text-muted font-monospace small\">%d</td>"
               "<td><strong>%s</strong><br>"
               "<small class=\"text-muted font-monospace\" style=\"font-size:.7rem\">%s</small>"
               "<input type=\"hidden\" name=\"entry\" value=\"%s\"></td>"
               "<td><input type=\"text\" name=\"mfr\" value=\"%s\""
               " class=\"form-control form-control-sm\" maxlength=\"32\"></td>"
               "<td><input type=\"text\" name=\"model\" value=\"%s\""
               " class=\"form-control form-control-sm\" maxlength=\"32\"></td>",
               ep++, name, topic, base_entry, mfr, model);
            if (is_onoff) {
                bool sw = (strcmp(subtype, "switch") == 0);
                HF("<td>"
                   "<select name=\"subtype\" class=\"form-select form-select-sm\">"
                   "<option value=\"light\"%s>"
                   "<i class=\"bi bi-lightbulb\"></i> Light</option>"
                   "<option value=\"switch\"%s>"
                   "<i class=\"bi bi-toggle-on\"></i> Switch</option>"
                   "</select></td>",
                   sw ? "" : " selected", sw ? " selected" : "");
            } else {
                HS("<td><input type=\"hidden\" name=\"subtype\" value=\"\"></td>");
            }
            HS("</tr>");
            if ((ep & 3) == 1) flush_chunk(req); /* flush every 4 rows */
        }
        HS("</tbody></table></div>"
           "<div class=\"card-footer text-end\">"
           "<button type=\"submit\" class=\"btn btn-sm btn-primary\">"
           "<i class=\"bi bi-check-lg me-1\"></i>");
        HS(t(L_PAIR_SAVE_ID));
        HS("</button></div></form></div>");
    } else {
        HS("<div class=\"alert alert-warning mb-3\">"
           "<i class=\"bi bi-exclamation-triangle me-2\"></i>");
        HS(t(L_PAIR_NO_DEVS));
        HS(" \xe2\x80\x94 <a href=\"/devices\">");
        HS(t(L_STATUS_CFG_DEVS));
        HS("</a>.</div>");
    }
    if (devs) cJSON_Delete(devs);

    flush_chunk(req);
    dash_page_close();
    flush_chunk(req);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t pair_open_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) return send_redirect(req, "/login");
    s_pair_open_until_us = esp_timer_get_time() + PAIR_WINDOW_US;
    return send_redirect(req, "/pair");
}

static esp_err_t pair_stop_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) return send_redirect(req, "/login");
    s_pair_open_until_us = 0;
    return send_redirect(req, "/pair");
}

static esp_err_t pair_unpair_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) return send_redirect(req, "/login");
    s_paired_ieee[0]     = '\0';
    s_paired_name[0]     = '\0';
    s_paired_model[0]    = '\0';
    s_pair_open_until_us = 0;
    return send_redirect(req, "/pair");
}

static esp_err_t pair_leave_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) return send_redirect(req, "/login");
    s_zb_pan_id     = 0;
    s_zb_channel    = 0;
    s_zb_short_addr = 0;
    /* Send the reboot page before triggering the reset so the browser
       gets a response instead of a connection error. */
    html_reset();
    page_common_head(t(L_RESTART_REBOOTING));
    HS("<div class=\"container py-5 text-center\" style=\"max-width:400px\">"
       "<div class=\"alert alert-info\">"
       "<i class=\"bi bi-arrow-clockwise fs-2\"></i>"
       "<h5 class=\"mt-2\">");
    HS(t(L_RESTART_REBOOTING));
    HS("</h5><p class=\"mb-0 small\">");
    HS(t(L_RESTART_RELOAD_MSG));
    HS("</p></div>"
       "<meta http-equiv=\"refresh\" content=\"10;url=/\"></div></body></html>");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, s_html, s_html_len);
    vTaskDelay(pdMS_TO_TICKS(400));
    zigbee_coordinator_leave_network();
    /* Zigbee alarm fires ~100 ms later and calls esp_zb_factory_reset() → esp_restart().
       Keep this task alive so the alarm can execute. */
    vTaskDelay(pdMS_TO_TICKS(1000));
    return ESP_OK;
}

/* ── Backup page ─────────────────────────────────────────────────────────── */

/* URL-decode a form field value, stopping at '&'. */
static void url_decode_field(char *dst, const char *src, size_t dst_size)
{
    char *d = dst;
    while (*src && *src != '&' && d < dst + dst_size - 1) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = {src[1], src[2], 0};
            *d++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            *d++ = (*src == '+') ? ' ' : *src;
            src++;
        }
    }
    *d = '\0';
}

/* Strip leading whitespace and optional surrounding quotes from a YAML scalar. */
static void yaml_strip_val(const char *s, char *out, size_t out_sz)
{
    while (*s == ' ' || *s == '\t') s++;
    char q = 0;
    if (*s == '\'' || *s == '"') { q = *s++; }
    size_t i = 0;
    while (*s && i < out_sz - 1) {
        if (q) {
            if (*s == q) {
                if (q == '\'' && *(s + 1) == '\'') { out[i++] = '\''; s += 2; continue; }
                break;
            }
        } else if (*s == '\r' || *s == '\n') break;
        out[i++] = *s++;
    }
    if (!q) while (i > 0 && (out[i-1] == ' ' || out[i-1] == '\t')) i--;
    out[i] = '\0';
}

/* Output one YAML "indent key: 'value'" line with single-quote escaping. */
static void yaml_kv(const char *indent, const char *key, const char *val)
{
    HF("%s%s: '", indent, key);
    for (const char *p = val ? val : ""; *p; p++) {
        if (*p == '\'') HS("''");
        else { char b[2] = {*p, '\0'}; HS(b); }
    }
    HS("'\n");
}

static esp_err_t backup_get_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) return send_redirect(req, "/login");
    if (is_default_creds())     return send_redirect(req, "/change-password");

    char query[128] = {0};
    bool imported = false;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char raw[4] = {0};
        if (httpd_query_key_value(query, "imported", raw, sizeof(raw)) == ESP_OK)
            imported = true;
    }

    dash_page_open(t(L_NAV_BACKUP), "/backup");
    if (imported) {
        HS("<div class=\"alert alert-success py-2\">"
           "<i class=\"bi bi-check-circle me-2\"></i>");
        HS(t(L_BACKUP_SUCCESS));
        HS("</div>");
    }

    HS("<div class=\"card mb-3\"><div class=\"card-header\"><h6 class=\"mb-0\">"
       "<i class=\"bi bi-download me-2\"></i>");
    HS(t(L_BTN_EXPORT));
    HS("</h6></div><div class=\"card-body\">"
       "<p class=\"text-muted small mb-3\">");
    HS(t(L_BACKUP_EXPORT_DESC));
    HS("</p><a href=\"/backup/export\" class=\"btn btn-sm btn-outline-primary\">"
       "<i class=\"bi bi-file-earmark-arrow-down me-1\"></i>");
    HS(t(L_BACKUP_DOWNLOAD));
    HS("</a></div></div>");

    HS("<div class=\"card\"><div class=\"card-header\"><h6 class=\"mb-0\">"
       "<i class=\"bi bi-upload me-2\"></i>");
    HS(t(L_BTN_IMPORT));
    HS("</h6></div><div class=\"card-body\">"
       "<p class=\"text-muted small mb-3\">");
    HS(t(L_BACKUP_IMPORT_DESC));
    HS("</p><form method=\"post\" action=\"/backup/import\">"
       "<div class=\"mb-3\">"
       "<textarea name=\"yaml\" rows=\"12\""
       " class=\"form-control form-control-sm font-monospace\""
       " placeholder=\"# Zigbee MQTT Gateway backup&#10;wifi:&#10;  ssid: ...\" required>"
       "</textarea></div>"
       "<button type=\"submit\" class=\"btn btn-sm btn-primary\">"
       "<i class=\"bi bi-upload me-1\"></i>");
    HS(t(L_BACKUP_APPLY_BTN));
    HS("</button></form></div></div>");

    dash_page_close();
    return send_html(req);
}

static esp_err_t export_yaml_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) return send_redirect(req, "/login");

    httpd_resp_set_type(req, "application/x-yaml");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"gateway_config.yaml\"");
    html_reset();
    HS("# Zigbee MQTT Gateway backup\n");
    HS("wifi:\n");
    yaml_kv("  ", "ssid",     app_config_get_wifi_ssid());
    yaml_kv("  ", "password", app_config_get_wifi_password());
    HS("mqtt:\n");
    yaml_kv("  ", "host",      app_config_get_mqtt_host());
    HF("  port: %d\n", app_config_get_mqtt_port() ? app_config_get_mqtt_port() : 1883);
    yaml_kv("  ", "client_id", app_config_get_mqtt_client_id());
    yaml_kv("  ", "username",  app_config_get_mqtt_username());
    yaml_kv("  ", "password",  app_config_get_mqtt_password());
    HS("devices:\n");
    flush_chunk(req);

    const char *dj = app_config_get_devices();
    cJSON *devs = dj ? cJSON_Parse(dj) : NULL;
    if (devs && cJSON_IsArray(devs)) {
        cJSON *item;
        cJSON_ArrayForEach(item, devs) {
            const char *raw = cJSON_GetStringValue(item);
            if (!raw) continue;
            char type_buf[32], topic[128], mfr[33], model[65];
            entry_get_type(raw, type_buf, sizeof(type_buf));
            entry_get_topic(raw, topic, sizeof(topic));
            entry_get_mfr(raw, mfr, sizeof(mfr));
            entry_get_model(raw, model, sizeof(model));
            HS("  - ");
            yaml_kv("", "type", type_buf);
            yaml_kv("    ", "topic",        topic);
            yaml_kv("    ", "manufacturer", mfr);
            yaml_kv("    ", "model",        model);
            flush_chunk(req);
        }
    }
    if (devs) cJSON_Delete(devs);

    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t import_post_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) return send_redirect(req, "/login");

    int total = req->content_len;
    if (total <= 0 || total > 8192)
        return send_redirect(req, "/backup?error=Body+too+large.");

    char *enc = malloc(total + 1);
    if (!enc) return send_redirect(req, "/backup?error=OOM.");
    int got = httpd_req_recv(req, enc, total);
    if (got <= 0) { free(enc); return send_redirect(req, "/backup?error=Read+error."); }
    enc[got] = '\0';

    const char *yp = strstr(enc, "yaml=");
    if (!yp) { free(enc); return send_redirect(req, "/backup?error=No+yaml+field."); }

    char *yaml = malloc(got + 1);
    if (!yaml) { free(enc); return send_redirect(req, "/backup?error=OOM."); }
    url_decode_field(yaml, yp + 5, got + 1);
    free(enc);

    /* Parse YAML line by line */
    typedef enum { S_NONE, S_WIFI, S_MQTT, S_DEV } sec_t;
    sec_t sec = S_NONE;
    bool in_dev = false;
    char dev_type[32]={0}, dev_topic[128]={0}, dev_mfr[33]={0}, dev_model[33]={0};
    char wifi_ssid[64]={0}, wifi_pass[64]={0};
    char mqtt_host[64]={0}, mqtt_port_s[8]={0}, mqtt_ci[32]={0};
    char mqtt_user[32]={0}, mqtt_pass[64]={0};
    cJSON *dev_arr = cJSON_CreateArray();

    char *line = strtok(yaml, "\n");
    while (line) {
        char *cr = strchr(line, '\r'); if (cr) *cr = '\0';
        const char *t = line; while (*t == ' ' || *t == '\t') t++;
        if (*t == '#' || *t == '\0') { line = strtok(NULL, "\n"); continue; }

        if (strcmp(t, "wifi:")    == 0) { sec = S_WIFI; in_dev = false; line = strtok(NULL, "\n"); continue; }
        if (strcmp(t, "mqtt:")    == 0) { sec = S_MQTT; in_dev = false; line = strtok(NULL, "\n"); continue; }
        if (strcmp(t, "devices:") == 0) { sec = S_DEV;  in_dev = false; line = strtok(NULL, "\n"); continue; }

        bool is_item = (*t == '-' && *(t + 1) == ' ');
        const char *kp  = is_item ? t + 2 : t;
        const char *col = strchr(kp, ':');
        if (!col) { line = strtok(NULL, "\n"); continue; }

        char key[32] = {0};
        size_t klen = (size_t)(col - kp);
        if (klen > 0 && klen < sizeof(key)) memcpy(key, kp, klen);
        size_t k = strlen(key); while (k > 0 && key[k-1] == ' ') key[--k] = '\0';

        char val[128] = {0};
        yaml_strip_val(col + 1, val, sizeof(val));

        if (sec == S_WIFI) {
            if (!strcmp(key, "ssid"))         strncpy(wifi_ssid, val, sizeof(wifi_ssid)-1);
            else if (!strcmp(key, "password")) strncpy(wifi_pass, val, sizeof(wifi_pass)-1);
        } else if (sec == S_MQTT) {
            if (!strcmp(key, "host"))          strncpy(mqtt_host,    val, sizeof(mqtt_host)-1);
            else if (!strcmp(key, "port"))     strncpy(mqtt_port_s,  val, sizeof(mqtt_port_s)-1);
            else if (!strcmp(key, "client_id"))strncpy(mqtt_ci,      val, sizeof(mqtt_ci)-1);
            else if (!strcmp(key, "username")) strncpy(mqtt_user,    val, sizeof(mqtt_user)-1);
            else if (!strcmp(key, "password")) strncpy(mqtt_pass,    val, sizeof(mqtt_pass)-1);
        } else if (sec == S_DEV) {
            if (is_item) {
                if (in_dev && dev_type[0] && dev_topic[0]) {
                    char entry[240];
                    snprintf(entry, sizeof(entry), "%s|%s|%s|%s",
                             dev_type, dev_topic,
                             dev_mfr[0] ? dev_mfr : "WB", dev_model);
                    cJSON_AddItemToArray(dev_arr, cJSON_CreateString(entry));
                }
                dev_type[0] = dev_topic[0] = dev_mfr[0] = dev_model[0] = '\0';
                in_dev = true;
            }
            if (!strcmp(key, "type"))              strncpy(dev_type,  val, sizeof(dev_type)-1);
            else if (!strcmp(key, "topic"))        strncpy(dev_topic, val, sizeof(dev_topic)-1);
            else if (!strcmp(key, "manufacturer")) strncpy(dev_mfr,   val, sizeof(dev_mfr)-1);
            else if (!strcmp(key, "model"))        strncpy(dev_model, val, sizeof(dev_model)-1);
        }
        line = strtok(NULL, "\n");
    }
    if (in_dev && dev_type[0] && dev_topic[0]) {
        char entry[240];
        snprintf(entry, sizeof(entry), "%s|%s|%s|%s",
                 dev_type, dev_topic,
                 dev_mfr[0] ? dev_mfr : "WB", dev_model);
        cJSON_AddItemToArray(dev_arr, cJSON_CreateString(entry));
    }
    free(yaml);

    if (wifi_ssid[0]) {
        app_config_save_wifi(wifi_ssid, wifi_pass);
        app_config_set_provisioned(true);
    }
    if (mqtt_host[0]) {
        int port = mqtt_port_s[0] ? atoi(mqtt_port_s) : 1883;
        if (port <= 0) port = 1883;
        if (!mqtt_ci[0]) snprintf(mqtt_ci, sizeof(mqtt_ci), "zigbee_gw_01");
        app_config_save_mqtt(mqtt_host, (uint16_t)port, false,
                             mqtt_ci, mqtt_user, mqtt_pass);
    }
    if (cJSON_GetArraySize(dev_arr) > 0) {
        char *js = cJSON_PrintUnformatted(dev_arr);
        if (js) { app_config_save_devices(js, strlen(js)); free(js); }
    }
    cJSON_Delete(dev_arr);
    return send_redirect(req, "/backup?imported=1");
}

/* ── API: WiFi networks (for JS async scan) ──────────────────────────────── */

static char s_json_response[4096];

static esp_err_t api_wifi_networks_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"done\":false,\"networks\":[]}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (!s_scan_done) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"done\":false,\"networks\":[]}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    int off = 0;
    off += snprintf(s_json_response + off, sizeof(s_json_response) - off,
                    "{\"done\":true,\"networks\":[");
    for (int i = 0; i < s_ap_count; i++) {
        if (i) off += snprintf(s_json_response + off,
                               sizeof(s_json_response) - off, ",");
        const char *auth_str;
        switch (s_ap_records[i].authmode) {
            case WIFI_AUTH_OPEN:     auth_str = "OPEN"; break;
            case WIFI_AUTH_WPA_PSK:  auth_str = "WPA";  break;
            case WIFI_AUTH_WPA2_PSK: auth_str = "WPA2"; break;
            case WIFI_AUTH_WPA3_PSK: auth_str = "WPA3"; break;
            default:                 auth_str = "WPA2"; break;
        }
        off += snprintf(s_json_response + off, sizeof(s_json_response) - off,
                        "{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":\"%s\",\"icon\":\"%s\"}",
                        (char *)s_ap_records[i].ssid,
                        s_ap_records[i].rssi,
                        auth_str,
                        rssi_icon(s_ap_records[i].rssi));
    }
    off += snprintf(s_json_response + off, sizeof(s_json_response) - off, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, s_json_response, off);
    return ESP_OK;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void web_server_set_zigbee_info(const char *ieee, uint16_t pan_id, uint8_t channel, uint16_t short_addr)
{
    if (ieee) strncpy(s_zb_ieee, ieee, sizeof(s_zb_ieee) - 1);
    s_zb_pan_id     = pan_id;
    s_zb_channel    = channel;
    s_zb_short_addr = short_addr;
}

void web_server_notify_paired(const char *name, const char *ieee, const char *model)
{
    if (name)  strncpy(s_paired_name,  name,  sizeof(s_paired_name)  - 1);
    if (ieee)  strncpy(s_paired_ieee,  ieee,  sizeof(s_paired_ieee)  - 1);
    if (model) strncpy(s_paired_model, model, sizeof(s_paired_model) - 1);
    s_pair_open_until_us = 0;
}

/* ── Firmware OTA page ───────────────────────────────────────────────────── */

static esp_err_t firmware_get_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) return send_redirect(req, "/login");
    if (is_default_creds())     return send_redirect(req, "/change-password");

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *update  = esp_ota_get_next_update_partition(NULL);
    esp_app_desc_t desc = {0};
    esp_ota_get_partition_description(running, &desc);

    char sha_short[17] = {0};
    for (int i = 0; i < 8; i++)
        snprintf(sha_short + i*2, 3, "%02x", desc.app_elf_sha256[i]);

    esp_app_desc_t other_desc = {0};
    bool has_other = update
        && esp_ota_get_partition_description(update, &other_desc) == ESP_OK
        && other_desc.magic_word == ESP_APP_DESC_MAGIC_WORD;

    char query[128] = {0}, err_msg[128] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char raw[128] = {0};
        httpd_query_key_value(query, "error", raw, sizeof(raw));
        url_decode(err_msg, raw, sizeof(err_msg));
    }

    dash_page_open(t(L_NAV_FIRMWARE), "/firmware");
    alert_div(err_msg, "danger");

    /* Running version */
    HS("<div class=\"card mb-3\"><div class=\"card-header\"><h6 class=\"mb-0\">"
       "<i class=\"bi bi-cpu me-2\"></i>");
    HS(t(L_FW_RUNNING));
    HS("</h6></div><div class=\"card-body p-0\">"
       "<table class=\"table table-sm mb-0\"><tbody>");
    HF("<tr><th class=\"ps-3 text-muted fw-normal\" style=\"width:40%%\">%s</th>"
       "<td class=\"fw-semibold\">%s</td></tr>",
       t(L_FW_VERSION), desc.version[0] ? desc.version : "\xe2\x80\x94");
    HF("<tr><th class=\"ps-3 text-muted fw-normal\">%s</th>"
       "<td>%s %s</td></tr>", t(L_FW_BUILT), desc.date, desc.time);
    HF("<tr><th class=\"ps-3 text-muted fw-normal\">IDF</th>"
       "<td class=\"font-monospace small\">%s</td></tr>", desc.idf_ver);
    HF("<tr><th class=\"ps-3 text-muted fw-normal\">%s</th>"
       "<td><span class=\"badge bg-primary\">%s</span></td></tr>",
       t(L_FW_SLOT), running ? running->label : "factory");
    HF("<tr><th class=\"ps-3 text-muted fw-normal\">SHA-256</th>"
       "<td class=\"font-monospace small\">%s\xe2\x80\xa6</td></tr>", sha_short);
    HS("</tbody></table></div></div>");

    /* Update target slot */
    if (update) {
        HS("<div class=\"card mb-3\"><div class=\"card-header py-2\"><h6 class=\"mb-0\">"
           "<i class=\"bi bi-arrow-repeat me-2\"></i>");
        HS(t(L_FW_TARGET));
        HS("</h6></div><div class=\"card-body p-0\">"
           "<table class=\"table table-sm mb-0\"><tbody>");
        HF("<tr><th class=\"ps-3 text-muted fw-normal\" style=\"width:40%%\">%s</th>"
           "<td><span class=\"badge bg-secondary\">%s</span></td></tr>",
           t(L_FW_SLOT), update->label);
        if (has_other)
            HF("<tr><th class=\"ps-3 text-muted fw-normal\">%s</th>"
               "<td class=\"small\">%s &mdash; %s %s</td></tr>",
               t(L_FW_PREVIOUS),
               other_desc.version[0] ? other_desc.version : "\xe2\x80\x94",
               other_desc.date, other_desc.time);
        else {
            HF("<tr><th class=\"ps-3 text-muted fw-normal\">%s</th>"
               "<td class=\"text-muted small\">%s</td></tr>",
               t(L_FW_PREVIOUS), t(L_FW_EMPTY));
        }
        HS("</tbody></table></div></div>");
    }

    /* Upload from file */
    HS("<div class=\"card mb-3\"><div class=\"card-header\"><h6 class=\"mb-0\">"
       "<i class=\"bi bi-upload me-2\"></i>");
    HS(t(L_FW_UPLOAD));
    HS("</h6></div><div class=\"card-body\">"
       "<p class=\"text-muted small mb-3\">");
    HS(t(L_FW_BIN_DESC));
    HS("</p>"
       "<input type=\"file\" id=\"fw-file\" accept=\".bin\""
       " class=\"form-control form-control-sm mb-3\">"
       "<div id=\"fw-status\" class=\"mb-3\" style=\"display:none\">"
       "<div class=\"progress mb-1\" style=\"height:8px\">"
       "<div id=\"fw-bar\" class=\"progress-bar\""
       " style=\"width:0%;transition:width .3s\"></div></div>"
       "<small id=\"fw-pct\" class=\"text-muted\">0%</small></div>"
       "<button id=\"fw-btn\" class=\"btn btn-sm btn-primary\""
       " onclick=\"fwUp()\">"
       "<i class=\"bi bi-cloud-upload me-1\"></i>");
    HS(t(L_FW_FLASH_BTN));
    HS("</button>"
       "<script>"
       "function fwUp(){"
       "var f=document.getElementById('fw-file').files[0];"
       "if(!f){alert('Select a .bin file first');return;}"
       "var btn=document.getElementById('fw-btn'),"
       "stat=document.getElementById('fw-status'),"
       "bar=document.getElementById('fw-bar'),"
       "pct=document.getElementById('fw-pct');"
       "btn.disabled=true;stat.style.display='block';"
       "pct.textContent='Uploading\xe2\x80\xa6';"
       "var xhr=new XMLHttpRequest();"
       "xhr.open('POST','/firmware/upload');"
       "xhr.setRequestHeader('Content-Type','application/octet-stream');"
       "xhr.upload.onprogress=function(e){"
       "if(e.lengthComputable){"
       "var p=Math.round(e.loaded/e.total*100);"
       "bar.style.width=p+'%';"
       "pct.textContent=p+'% ('+Math.round(e.loaded/1024)+' KB)';}};"
       "xhr.onload=function(){"
       "if(xhr.status===200){"
       "pct.textContent='Done \xe2\x80\x94 rebooting\xe2\x80\xa6';"
       "bar.style.width='100%';"
       "bar.className='progress-bar bg-success';"
       "setTimeout(function(){window.location.href='/';},15000);"
       "}else{"
       "pct.textContent='Server error '+xhr.status;"
       "btn.disabled=false;}};"
       "xhr.onerror=function(){pct.textContent='Network error';btn.disabled=false;};"
       "xhr.send(f);}"
       "</script>"
       "</div></div>");

    /* Fetch from URL */
    HS("<div class=\"card\"><div class=\"card-header\"><h6 class=\"mb-0\">"
       "<i class=\"bi bi-link-45deg me-2\"></i>");
    HS(t(L_FW_FETCH_URL));
    HS("</h6></div><div class=\"card-body\">"
       "<p class=\"text-muted small mb-3\">");
    HS(t(L_FW_FETCH_DESC));
    HS("</p><form method=\"post\" action=\"/firmware/fetch\""
       " onsubmit=\"var b=this.querySelector('button');"
       "b.disabled=true;"
       "b.innerHTML='<span class=\\'spinner-border spinner-border-sm me-1\\'></span>"
       "Fetching\xe2\x80\xa6';\">"
       "<div class=\"input-group input-group-sm\">"
       "<input type=\"url\" name=\"url\" class=\"form-control\""
       " placeholder=\"http://192.168.1.x/zigbee-mqtt-gateway.bin\" required>"
       "<button type=\"submit\" class=\"btn btn-outline-primary\">"
       "<i class=\"bi bi-download me-1\"></i>");
    HS(t(L_FW_FETCH_BTN));
    HS("</button></div></form></div></div>");

    dash_page_close();
    return send_html(req);
}

static esp_err_t firmware_upload_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) return send_redirect(req, "/login");

    int total = req->content_len;
    if (total < 65536)
        return send_redirect(req, "/firmware?error=Invalid+or+missing+binary.");

    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update)
        return send_redirect(req, "/firmware?error=No+OTA+partition+found.");

    esp_ota_handle_t ota;
    if (esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK)
        return send_redirect(req, "/firmware?error=OTA+begin+failed.");

    char buf[1024];
    int remaining = total;
    bool ok = true;
    while (remaining > 0) {
        int chunk = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int got = httpd_req_recv(req, buf, chunk);
        if (got <= 0) { ok = false; break; }
        if (esp_ota_write(ota, buf, (size_t)got) != ESP_OK) { ok = false; break; }
        remaining -= got;
    }

    if (!ok) {
        esp_ota_abort(ota);
        return send_redirect(req, "/firmware?error=Upload+write+failed.");
    }
    if (esp_ota_end(ota) != ESP_OK)
        return send_redirect(req, "/firmware?error=Image+validation+failed.");
    if (esp_ota_set_boot_partition(update) != ESP_OK)
        return send_redirect(req, "/firmware?error=Set+boot+partition+failed.");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, "", 0); /* 200 OK, body handled by JS */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t firmware_fetch_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) return send_redirect(req, "/login");
    if (!form_read(req))
        return send_redirect(req, "/firmware?error=Read+error.");

    char url[256] = {0};
    form_get(s_form_buf, "url", url, sizeof(url));
    if (!url[0])
        return send_redirect(req, "/firmware?error=No+URL+provided.");

    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update)
        return send_redirect(req, "/firmware?error=No+OTA+partition+found.");

    esp_http_client_config_t cfg = {
        .url            = url,
        .timeout_ms     = 30000,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client)
        return send_redirect(req, "/firmware?error=HTTP+client+init+failed.");

    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        return send_redirect(req, "/firmware?error=HTTP+connect+failed.");
    }

    int content_len = esp_http_client_fetch_headers(client);
    if (content_len < 65536) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return send_redirect(req, "/firmware?error=URL+returned+too+little+data.");
    }

    esp_ota_handle_t ota;
    if (esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return send_redirect(req, "/firmware?error=OTA+begin+failed.");
    }

    static char dl_buf[2048];
    int total = 0;
    bool ok = true;
    while (true) {
        int len = esp_http_client_read(client, dl_buf, sizeof(dl_buf));
        if (len < 0) { ok = false; break; }
        if (len == 0) break;
        if (esp_ota_write(ota, dl_buf, (size_t)len) != ESP_OK) { ok = false; break; }
        total += len;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (!ok || total < 65536) {
        esp_ota_abort(ota);
        return send_redirect(req, "/firmware?error=Download+or+write+failed.");
    }
    if (esp_ota_end(ota) != ESP_OK)
        return send_redirect(req, "/firmware?error=Image+validation+failed.");
    if (esp_ota_set_boot_partition(update) != ESP_OK)
        return send_redirect(req, "/firmware?error=Set+boot+partition+failed.");

    dash_page_open(t(L_NAV_FIRMWARE), "/firmware");
    HS("<div class=\"alert alert-success py-3 text-center\">"
       "<h5 class=\"mb-1\"><i class=\"bi bi-check-circle-fill me-2\"></i>");
    HS(t(L_FW_SUCCESS));
    HF(" (%d KB)</h5><p class=\"mb-0 small\">", total / 1024);
    HS(t(L_FW_REBOOT_MSG));
    HS("</p></div><meta http-equiv=\"refresh\" content=\"15;url=/\">");
    dash_page_close();
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, s_html, s_html_len);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* ── Restart handler ─────────────────────────────────────────────────────── */

static esp_err_t restart_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) return send_redirect(req, "/login");
    html_reset();
    page_common_head(t(L_RESTART_REBOOTING));
    HS("<div class=\"container py-5 text-center\" style=\"max-width:400px\">"
       "<div class=\"alert alert-info\">"
       "<i class=\"bi bi-arrow-clockwise fs-2\"></i>"
       "<h5 class=\"mt-2\">");
    HS(t(L_RESTART_REBOOTING));
    HS("</h5><p class=\"mb-0 small\">");
    HS(t(L_RESTART_RELOAD_MSG));
    HS("</p></div>"
       "<meta http-equiv=\"refresh\" content=\"10;url=/\"></div></body></html>");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, s_html, s_html_len);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

/* ── Language handler ────────────────────────────────────────────────────── */

static esp_err_t lang_post_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) return send_redirect(req, "/login");
    if (!form_read(req)) return send_redirect(req, "/status");
    char lang_val[4] = {0};
    form_get(s_form_buf, "lang", lang_val, sizeof(lang_val));
    lang_t l = (strcmp(lang_val, "ru") == 0) ? LANG_RU : LANG_EN;
    locale_set(l);
    app_config_set_language(l == LANG_RU ? "ru" : "en");
    /* Redirect back to referring page if possible. */
    char ref[64] = "/status";
    httpd_req_get_hdr_value_str(req, "Referer", ref, sizeof(ref));
    return send_redirect(req, ref);
}

/* ── Server start / stop ─────────────────────────────────────────────────── */

static const httpd_uri_t s_uris[] = {
    {.uri = "/",                      .method = HTTP_GET,  .handler = root_handler},
    {.uri = "/favicon.ico",           .method = HTTP_GET,  .handler = favicon_handler},
    {.uri = "/css/bootstrap.min.css", .method = HTTP_GET,  .handler = css_bootstrap_handler},
    {.uri = "/css/bootstrap-icons.css",.method = HTTP_GET, .handler = css_icons_handler},
    {.uri = "/css/fonts/bootstrap-icons.woff2",.method = HTTP_GET, .handler = font_handler},
    {.uri = "/js/enhance.js",         .method = HTTP_GET,  .handler = enhance_js_handler},
    {.uri = "/login",                 .method = HTTP_GET,  .handler = login_get_handler},
    {.uri = "/login",                 .method = HTTP_POST, .handler = login_post_handler},
    {.uri = "/logout",                .method = HTTP_POST, .handler = logout_handler},
    {.uri = "/change-password",       .method = HTTP_GET,  .handler = change_password_get_handler},
    {.uri = "/change-password",       .method = HTTP_POST, .handler = change_password_post_handler},
    {.uri = "/status",                .method = HTTP_GET,  .handler = status_handler},
    {.uri = "/wifi",                  .method = HTTP_GET,  .handler = wifi_get_handler},
    {.uri = "/wifi/scan",             .method = HTTP_GET,  .handler = wifi_scan_handler},
    {.uri = "/wifi/scan",             .method = HTTP_POST, .handler = wifi_scan_handler},
    {.uri = "/wifi",                  .method = HTTP_POST, .handler = wifi_post_handler},
    {.uri = "/wifi/connecting",       .method = HTTP_GET,  .handler = wifi_connecting_handler},
    {.uri = "/wifi/connected",        .method = HTTP_GET,  .handler = wifi_connected_handler},
    {.uri = "/mqtt",                  .method = HTTP_GET,  .handler = mqtt_get_handler},
    {.uri = "/mqtt",                  .method = HTTP_POST, .handler = mqtt_post_handler},
    {.uri = "/devices",               .method = HTTP_GET,  .handler = devices_get_handler},
    {.uri = "/devices/scan",          .method = HTTP_GET,  .handler = devices_scan_handler},
    {.uri = "/devices",               .method = HTTP_POST, .handler = devices_post_handler},
    {.uri = "/devices/configure",     .method = HTTP_GET,  .handler = configure_get_handler},
    {.uri = "/devices/configure",     .method = HTTP_POST, .handler = configure_post_handler},
    {.uri = "/pair",                  .method = HTTP_GET,  .handler = pair_get_handler},
    {.uri = "/pair/open",             .method = HTTP_POST, .handler = pair_open_handler},
    {.uri = "/pair/stop",             .method = HTTP_POST, .handler = pair_stop_handler},
    {.uri = "/pair/unpair",           .method = HTTP_POST, .handler = pair_unpair_handler},
    {.uri = "/pair/leave",            .method = HTTP_POST, .handler = pair_leave_handler},
    {.uri = "/backup",                .method = HTTP_GET,  .handler = backup_get_handler},
    {.uri = "/backup/export",         .method = HTTP_GET,  .handler = export_yaml_handler},
    {.uri = "/backup/import",         .method = HTTP_POST, .handler = import_post_handler},
    {.uri = "/firmware",              .method = HTTP_GET,  .handler = firmware_get_handler},
    {.uri = "/firmware/upload",       .method = HTTP_POST, .handler = firmware_upload_handler},
    {.uri = "/firmware/fetch",        .method = HTTP_POST, .handler = firmware_fetch_handler},
    {.uri = "/restart",               .method = HTTP_POST, .handler = restart_handler},
    {.uri = "/lang",                  .method = HTTP_POST, .handler = lang_post_handler},
    {.uri = "/api/wifi/networks",     .method = HTTP_GET,  .handler = api_wifi_networks_handler},
};
#define URI_COUNT (sizeof(s_uris) / sizeof(s_uris[0]))

void web_server_start(uint16_t port)
{
    if (s_server) return;

    const char *saved_lang = app_config_get_language();
    locale_set((strcmp(saved_lang, "ru") == 0) ? LANG_RU : LANG_EN);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port      = port;
    config.max_uri_handlers = URI_COUNT + 2;
    config.stack_size       = 12288;
    config.lru_purge_enable = true;
    config.max_open_sockets = 4;

    ESP_LOGI(TAG, "Free heap before httpd_start: %lu bytes", esp_get_free_heap_size());
    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server (free heap: %lu)", esp_get_free_heap_size());
        return;
    }
    for (size_t i = 0; i < URI_COUNT; i++)
        httpd_register_uri_handler(s_server, &s_uris[i]);

    ESP_LOGI(TAG, "Web server started on port %d (%zu handlers, free heap: %lu)",
             port, URI_COUNT, esp_get_free_heap_size());
}

void web_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
