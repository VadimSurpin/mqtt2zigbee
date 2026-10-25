#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "mqtt_client_wrapper.h"
#include "z2m_driver.h"

static const char *TAG = "Z2M_DRIVER";

/* zigbee2mqtt/bridge/devices is a retained JSON array that can reach hundreds of KB, so it
 * is scanned as it streams in (a few hundred bytes of state) instead of being buffered.
 * Shape (only what is needed):
 *   [ { "friendly_name":"x", "type":"Router",
 *       "definition": { "model":"..", "vendor":"..",
 *           "exposes":[ { "type":"switch", "features":[ {"property":"state","access":7} ] },
 *                       { "property":"temperature", "access":1 }, ... ] } }, ... ]
 * A leaf is an object under "exposes"/"features" that has a "property" and no "features". */

#define MAXD      12
#define PEND_MAX  24

typedef enum { R_OTHER, R_DEVLIST, R_DEVICE, R_DEFINITION, R_EXPOSES, R_EXPOSE, R_FEATURES } role_t;

typedef struct {
    role_t role;
    bool   is_arr;
    bool   in_value;       /* object: between ':' and the next ',' */
    char   key[16];        /* object: last key read */
    char   prop[20];       /* R_EXPOSE */
    bool   has_prop;
    bool   has_features;
    int    access;
} ctx_t;

static struct {
    ctx_t ctx[MAXD];
    int   depth;                    /* open containers; may exceed MAXD (then ignored) */
    bool  in_str, esc;
    int   uskip;                    /* hex digits left to skip after \u */
    char  sbuf[44];
    int   slen;
    bool  s_trunc;
    char  tok[16];
    int   tlen;
    bool  in_tok;
    /* current device */
    char  name[Z2M_NAME_LEN];
    bool  name_bad, coord;
    char  vendor[16], model[24];
    struct { char prop[20]; int access; } pend[PEND_MAX];
    int   npend;
} P;

static z2m_entry_t *s_tab = NULL;
static int  s_count = 0;
static bool s_ready = false;
static bool s_truncated = false;

/* ── Classification ───────────────────────────────────────────────────────── */

static z2m_kind_t classify(const char *prop, bool has_brightness)
{
    if (strcmp(prop, "state") == 0 || strncmp(prop, "state_l", 7) == 0)
        return has_brightness ? Z2M_KIND_LIGHT : Z2M_KIND_SWITCH;
    if (strcmp(prop, "temperature") == 0) return Z2M_KIND_TEMPERATURE;
    if (strcmp(prop, "humidity") == 0)    return Z2M_KIND_HUMIDITY;
    if (strcmp(prop, "pressure") == 0)    return Z2M_KIND_PRESSURE;
    if (strcmp(prop, "contact") == 0)     return Z2M_KIND_CONTACT;
    if (strcmp(prop, "illuminance_lux") == 0 || strcmp(prop, "illuminance") == 0 ||
        strcmp(prop, "battery") == 0 || strcmp(prop, "power") == 0 ||
        strcmp(prop, "co2") == 0)
        return Z2M_KIND_ANALOG;
    return Z2M_KIND_NONE;
}

const char *z2m_kind_name(uint8_t kind)
{
    switch (kind) {
    case Z2M_KIND_SWITCH:      return "switch";
    case Z2M_KIND_LIGHT:       return "light";
    case Z2M_KIND_TEMPERATURE: return "temperature";
    case Z2M_KIND_HUMIDITY:    return "humidity";
    case Z2M_KIND_PRESSURE:    return "pressure";
    case Z2M_KIND_CONTACT:     return "contact";
    case Z2M_KIND_ANALOG:      return "analog";
    default:                   return "";
    }
}

/* ── Device bookkeeping ───────────────────────────────────────────────────── */

static void device_begin(void)
{
    P.name[0] = P.vendor[0] = P.model[0] = '\0';
    P.name_bad = P.coord = false;
    P.npend = 0;
}

static void device_end(void)
{
    if (!s_tab || P.coord || P.name_bad || !P.name[0] || P.npend == 0) return;

    bool bright = false;
    for (int i = 0; i < P.npend; i++)
        if (strcmp(P.pend[i].prop, "brightness") == 0) bright = true;

    for (int i = 0; i < P.npend; i++) {
        z2m_kind_t k = classify(P.pend[i].prop, bright);
        if (k == Z2M_KIND_NONE) continue;
        bool writable = (P.pend[i].access >= 0) && (P.pend[i].access & 2);
        /* An on/off state that cannot be set is not controllable — skip it. */
        if ((k == Z2M_KIND_SWITCH || k == Z2M_KIND_LIGHT) && !writable) continue;
        if (s_count >= Z2M_MAX_ENTRIES) { s_truncated = true; return; }

        z2m_entry_t *e = &s_tab[s_count++];
        memset(e, 0, sizeof(*e));
        strncpy(e->device, P.name,   sizeof(e->device) - 1);
        strncpy(e->prop,   P.pend[i].prop, sizeof(e->prop) - 1);
        strncpy(e->vendor, P.vendor, sizeof(e->vendor) - 1);
        strncpy(e->model,  P.model,  sizeof(e->model) - 1);
        e->kind = (uint8_t)k;
        e->writable = writable;
    }
}

static void expose_end(const ctx_t *c)
{
    if (c->has_prop && !c->has_features && P.npend < PEND_MAX) {
        strncpy(P.pend[P.npend].prop, c->prop, sizeof(P.pend[0].prop) - 1);
        P.pend[P.npend].prop[sizeof(P.pend[0].prop) - 1] = '\0';
        P.pend[P.npend].access = c->access;
        P.npend++;
    }
}

/* ── Streaming JSON scanner ───────────────────────────────────────────────── */

static ctx_t *cur(void)
{
    return (P.depth > 0 && P.depth <= MAXD) ? &P.ctx[P.depth - 1] : NULL;
}

static void open_container(char c)
{
    ctx_t *parent = cur();
    role_t role = R_OTHER;

    if (P.depth == 0) {
        if (c == '[') role = R_DEVLIST;
    } else if (parent) {
        if (parent->is_arr) {
            if (c == '{') {
                if (parent->role == R_DEVLIST) role = R_DEVICE;
                else if (parent->role == R_EXPOSES || parent->role == R_FEATURES) role = R_EXPOSE;
            }
        } else if (parent->in_value) {
            if (parent->role == R_DEVICE && c == '{' && strcmp(parent->key, "definition") == 0)
                role = R_DEFINITION;
            else if (parent->role == R_DEFINITION && c == '[' && strcmp(parent->key, "exposes") == 0)
                role = R_EXPOSES;
            else if (parent->role == R_EXPOSE && c == '[' && strcmp(parent->key, "features") == 0) {
                role = R_FEATURES;
                parent->has_features = true;
            }
        }
    }

    P.depth++;
    if (P.depth <= MAXD) {
        ctx_t *n = &P.ctx[P.depth - 1];
        memset(n, 0, sizeof(*n));
        n->role = role;
        n->is_arr = (c == '[');
        n->access = -1;
        if (role == R_DEVICE) device_begin();
    }
}

static void close_container(void)
{
    if (P.depth <= 0) return;
    ctx_t *c = cur();
    if (c) {
        if (c->role == R_EXPOSE) expose_end(c);
        else if (c->role == R_DEVICE) device_end();
    }
    P.depth--;
}

static void string_value(ctx_t *c)
{
    switch (c->role) {
    case R_DEVICE:
        if (strcmp(c->key, "friendly_name") == 0) {
            /* Names that were cut, or contain the entry separators, cannot be addressed. */
            P.name_bad = P.s_trunc || P.slen >= Z2M_NAME_LEN ||
                         strchr(P.sbuf, '|') || strchr(P.sbuf, '#');
            strncpy(P.name, P.sbuf, sizeof(P.name) - 1);
            P.name[sizeof(P.name) - 1] = '\0';
        } else if (strcmp(c->key, "type") == 0) {
            P.coord = (strcmp(P.sbuf, "Coordinator") == 0);
        }
        break;
    case R_DEFINITION:
        if (strcmp(c->key, "model") == 0) {
            strncpy(P.model, P.sbuf, sizeof(P.model) - 1);
            P.model[sizeof(P.model) - 1] = '\0';
        } else if (strcmp(c->key, "vendor") == 0) {
            strncpy(P.vendor, P.sbuf, sizeof(P.vendor) - 1);
            P.vendor[sizeof(P.vendor) - 1] = '\0';
        }
        break;
    case R_EXPOSE:
        if (strcmp(c->key, "property") == 0 && !P.s_trunc && P.slen < (int)sizeof(c->prop)) {
            strncpy(c->prop, P.sbuf, sizeof(c->prop) - 1);
            c->has_prop = true;
        }
        break;
    default:
        break;
    }
}

static void string_close(void)
{
    ctx_t *c = cur();
    if (!c) return;
    if (!c->is_arr && !c->in_value) {
        strncpy(c->key, P.sbuf, sizeof(c->key) - 1);
        c->key[sizeof(c->key) - 1] = '\0';
    } else if (!c->is_arr) {
        string_value(c);
    }
}

static void token_close(void)
{
    ctx_t *c = cur();
    P.tok[P.tlen] = '\0';
    if (c && !c->is_arr && c->in_value && c->role == R_EXPOSE && strcmp(c->key, "access") == 0)
        c->access = atoi(P.tok);
}

static void str_add(char ch)
{
    if (P.slen < (int)sizeof(P.sbuf) - 1) P.sbuf[P.slen++] = ch;
    else P.s_trunc = true;
    P.sbuf[P.slen] = '\0';
}

static void feed(const char *p, int n)
{
    for (int i = 0; i < n; i++) {
        char ch = p[i];

        if (P.in_str) {
            if (P.uskip > 0) { P.uskip--; continue; }
            if (P.esc) {
                P.esc = false;
                if (ch == 'u') { P.uskip = 4; str_add('?'); }
                else str_add(ch);
                continue;
            }
            if (ch == '\\') { P.esc = true; continue; }
            if (ch == '"')  { P.in_str = false; P.sbuf[P.slen] = '\0'; string_close(); continue; }
            str_add(ch);
            continue;
        }

        if (P.in_tok) {
            if (ch == ',' || ch == '}' || ch == ']' || ch == ' ' || ch == '\n' ||
                ch == '\r' || ch == '\t') {
                token_close();
                P.in_tok = false;
            } else {
                if (P.tlen < (int)sizeof(P.tok) - 1) P.tok[P.tlen++] = ch;
                continue;
            }
        }

        switch (ch) {
        case '"': P.in_str = true; P.slen = 0; P.sbuf[0] = '\0'; P.s_trunc = false; break;
        case '{':
        case '[': open_container(ch); break;
        case '}':
        case ']': close_container(); break;
        case ',': { ctx_t *c = cur(); if (c && !c->is_arr) c->in_value = false; break; }
        case ':': { ctx_t *c = cur(); if (c && !c->is_arr) c->in_value = true;  break; }
        case ' ': case '\n': case '\r': case '\t': break;
        default:  P.in_tok = true; P.tlen = 0; P.tok[P.tlen++] = ch; break;
        }
    }
}

static void stream_cb(const char *data, int len, int offset, int total)
{
    if (offset == 0) {
        memset(&P, 0, sizeof(P));
        s_count = 0;
        s_ready = false;
        s_truncated = false;
    }
    feed(data, len);
    if (offset + len >= total) {
        s_ready = true;
        ESP_LOGI(TAG, "bridge/devices parsed: %d properties%s", s_count,
                 s_truncated ? " (table full)" : "");
    }
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void z2m_driver_refresh(void)
{
    if (!s_tab) {
        s_tab = calloc(Z2M_MAX_ENTRIES, sizeof(z2m_entry_t));
        if (!s_tab) {
            ESP_LOGE(TAG, "Cannot allocate %u bytes for the Zigbee2MQTT table",
                     (unsigned)(Z2M_MAX_ENTRIES * sizeof(z2m_entry_t)));
            return;
        }
    }
    s_count = 0;
    s_ready = false;
    s_truncated = false;
    memset(&P, 0, sizeof(P));
    mqtt_client_stream_topic(Z2M_BASE_TOPIC "/bridge/devices", stream_cb);
}

void z2m_driver_release(void)
{
    mqtt_client_stream_topic(NULL, NULL);
    free(s_tab);
    s_tab = NULL;
    s_count = 0;
    s_ready = false;
}

int z2m_driver_get_entries(const z2m_entry_t **out)
{
    if (out) *out = s_tab;
    return s_tab ? s_count : 0;
}

bool z2m_driver_ready(void)     { return s_ready; }
bool z2m_driver_truncated(void) { return s_truncated; }
