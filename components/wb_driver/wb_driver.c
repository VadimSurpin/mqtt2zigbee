#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "wb_driver.h"
#include "mqtt_client_wrapper.h"

static const char *TAG = "WB";

static wb_device_t  s_devices[WB_MAX_DEVICES];  /* static BSS — ~8 KB */
static wb_control_t *s_controls = NULL;           /* heap only during discovery */
static bool s_discovery_active  = false;
static bool s_table_full_warned = false;
static esp_timer_handle_t s_discovery_timer = NULL;

#define DISCOVERY_WINDOW_US  (60ULL * 1000000ULL)

static void discovery_timeout_cb(void *arg)
{
    wb_driver_disable_discovery();
}

/* ── Type tables ──────────────────────────────────────────────────────────── */

static const char *const s_type_names[] = {
    "—",                      /* 0: WB_TYPE_UNKNOWN */
    "switch",                 /* 1 */
    "range",                  /* 2 */
    "pushbutton",             /* 3 */
    "rgb",                    /* 4 */
    "text",                   /* 5 */
    "alarm",                  /* 6 */
    "temperature",            /* 7 */
    "rel_humidity",           /* 8 */
    "atmospheric_pressure",   /* 9 */
    "lux",                    /* 10 */
    "power",                  /* 11 */
    "power_consumption",      /* 12 */
    "voltage",                /* 13 */
    "current",                /* 14 */
    "value",                  /* 15 */
    "energy",                 /* 16 */
    "resistance",             /* 17 */
    "concentration",          /* 18 */
    "heat_energy",            /* 19 */
    "water_flow",             /* 20 */
    "water_consumption",      /* 21 */
    "wind_speed",             /* 22 */
    "wind_direction",         /* 23 */
    "noise",                  /* 24 */
    "reactive_power",         /* 25 */
    "pm2.5",                  /* 26 */
    "pm10",                   /* 27 */
    "precipitation_rate",     /* 28 */
};
#define WB_TYPE_COUNT (sizeof(s_type_names) / sizeof(s_type_names[0]))

static uint8_t wb_type_from_string(const char *type)
{
    if (!type || !type[0]) return WB_TYPE_UNKNOWN;
    for (uint8_t i = 1; i < (uint8_t)WB_TYPE_COUNT; i++) {
        if (strcmp(type, s_type_names[i]) == 0) return i;
    }
    return WB_TYPE_UNKNOWN;
}

const char *wb_control_type_name(const wb_control_t *ctrl)
{
    if (ctrl->type_id < (uint8_t)WB_TYPE_COUNT) return s_type_names[ctrl->type_id];
    return "—";
}

bool wb_control_is_writable(const wb_control_t *ctrl)
{
    return !(ctrl->flags & WB_FLAG_READONLY);
}

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static wb_device_t *find_device(const char *id)
{
    for (int i = 0; i < WB_MAX_DEVICES; i++) {
        if (s_devices[i].valid && strcmp(s_devices[i].id, id) == 0)
            return &s_devices[i];
    }
    return NULL;
}

static wb_device_t *find_or_add_device(const char *id)
{
    wb_device_t *d = find_device(id);
    if (d) return d;
    for (int i = 0; i < WB_MAX_DEVICES; i++) {
        if (!s_devices[i].valid) {
            strncpy(s_devices[i].id,   id, sizeof(s_devices[i].id)   - 1);
            strncpy(s_devices[i].name, id, sizeof(s_devices[i].name) - 1);
            s_devices[i].valid = true;
            return &s_devices[i];
        }
    }
    return NULL;
}

static wb_control_t *find_control_by_idx(uint8_t dev_idx, const char *name)
{
    if (!s_controls) return NULL;
    for (int i = 0; i < WB_MAX_CONTROLS; i++) {
        if ((s_controls[i].flags & WB_FLAG_VALID) &&
            s_controls[i].dev_idx == dev_idx &&
            strcmp(s_controls[i].name, name) == 0)
            return &s_controls[i];
    }
    return NULL;
}

static wb_control_t *find_or_add_control(uint8_t dev_idx, const char *name)
{
    if (!s_controls) {
        ESP_LOGW(TAG, "Control '%s' ignored — no control table (discovery inactive?)", name);
        return NULL;
    }

    wb_control_t *existing = find_control_by_idx(dev_idx, name);
    if (existing) return existing;

    if (!s_discovery_active) {
        ESP_LOGD(TAG, "Control '%s' ignored — discovery window closed", name);
        return NULL;
    }

    for (int i = 0; i < WB_MAX_CONTROLS; i++) {
        if (!(s_controls[i].flags & WB_FLAG_VALID)) {
            s_controls[i].dev_idx  = dev_idx;
            strncpy(s_controls[i].name, name, sizeof(s_controls[i].name) - 1);
            s_controls[i].type_id  = WB_TYPE_UNKNOWN;
            s_controls[i].order    = 0;
            s_controls[i].units[0] = '\0';
            s_controls[i].flags    = WB_FLAG_VALID | WB_FLAG_READONLY; /* readonly until proven otherwise */
            ESP_LOGI(TAG, "New control dev=%u name='%s'", dev_idx, s_controls[i].name);
            return &s_controls[i];
        }
    }

    if (!s_table_full_warned) {
        ESP_LOGW(TAG, "Control table full (%d slots) — increase WB_MAX_CONTROLS",
                 WB_MAX_CONTROLS);
        s_table_full_warned = true;
    }
    ESP_LOGW(TAG, "Dropped control %u/'%s' — table full", dev_idx, name);
    return NULL;
}

/* Store the type_id.  Do NOT touch the readonly flag here.
 * The same type string (e.g. "switch") is used for both writable relay
 * outputs and read-only physical inputs on devices like wb-mr.
 * Writability is determined solely by:
 *   1. retained /on message (intercepted at the top of wb_driver_process)
 *   2. explicit /meta/readonly field in the control meta JSON */
static void apply_type(wb_control_t *ctrl, uint8_t type_id)
{
    ctrl->type_id = type_id;
}

static bool parse_wb_topic(const char *topic,
                            char *device_id,
                            char *ctrl_name,
                            bool *is_meta,
                            char *meta_field)
{
    const char *p = topic;
    if (*p == '/') p++;
    if (strncmp(p, "devices/", 8) != 0) return false;
    p += 8;

    const char *slash = strchr(p, '/');
    if (!slash) {
        int len = strlen(p);
        if (len == 0 || len >= 32) return false;
        strncpy(device_id, p, 31);
        device_id[len] = '\0';
        ctrl_name[0] = '\0';
        *is_meta = false;
        meta_field[0] = '\0';
        return true;
    }

    int dev_len = slash - p;
    if (dev_len == 0 || dev_len >= 32) return false;
    strncpy(device_id, p, dev_len);
    device_id[dev_len] = '\0';
    p = slash + 1;

    if (strncmp(p, "meta", 4) == 0) {
        ctrl_name[0] = '\0';
        *is_meta = true;
        strncpy(meta_field, p + 4, 31);
        meta_field[31] = '\0';
        return true;
    }

    if (strncmp(p, "controls/", 9) != 0) return false;
    p += 9;

    /* skip write-command topics — /on is handled before parse_wb_topic is called */
    size_t plen = strlen(p);
    if (plen >= 3 && strcmp(p + plen - 3, "/on") == 0) return false;

    const char *meta_pos = strstr(p, "/meta");
    if (!meta_pos) {
        strncpy(ctrl_name, p, sizeof(((wb_control_t*)0)->name) - 1);
        ctrl_name[sizeof(((wb_control_t*)0)->name) - 1] = '\0';
        *is_meta = false;
        meta_field[0] = '\0';
    } else {
        int name_len = meta_pos - p;
        if (name_len <= 0 || name_len >= (int)sizeof(((wb_control_t*)0)->name)) return false;
        strncpy(ctrl_name, p, name_len);
        ctrl_name[name_len] = '\0';
        *is_meta = true;
        strncpy(meta_field, meta_pos + 5, 31);
        meta_field[31] = '\0';
    }
    return true;
}

/* ── Built-in Wiren Board virtual devices (excluded from discovery) ───────── */

static const char *const s_builtin_ids[] = {
    "system", "hwmon", "power_status", "network", "alarms", "wbrules",
    "wb-gpio", "wb-adc", "wb-w1", "metrics", "gsm",
};

bool wb_driver_is_builtin(const char *dev_id)
{
    if (!dev_id) return false;
    /* 1-Wire sensors are published as "28-xxxxxxxxxxxx" */
    if (strncmp(dev_id, "28-", 3) == 0) return true;
    for (size_t i = 0; i < sizeof(s_builtin_ids) / sizeof(s_builtin_ids[0]); i++) {
        if (strcmp(dev_id, s_builtin_ids[i]) == 0) return true;
    }
    return false;
}

static uint8_t s_mode = 0;          /* 0 WB, 1 WB + built-ins, 2 none (Zigbee2MQTT) */
static bool    s_wb_present = false;

void    wb_driver_set_mode(uint8_t mode) { s_mode = mode; }
uint8_t wb_driver_get_mode(void)         { return s_mode; }
bool    wb_driver_wb_present(void)       { return s_wb_present; }

static bool is_excluded(const char *dev_id)
{
    if (s_mode >= 2) return true;
    return s_mode == 0 && wb_driver_is_builtin(dev_id);
}

/* ── Discovery lifecycle ──────────────────────────────────────────────────── */

void wb_driver_enable_discovery(void)
{
    if (!s_controls) {
        s_controls = calloc(WB_MAX_CONTROLS, sizeof(wb_control_t));
        if (!s_controls) {
            ESP_LOGE(TAG, "Failed to allocate control table (%u bytes)",
                     (unsigned)(WB_MAX_CONTROLS * sizeof(wb_control_t)));
            return;
        }
    } else {
        memset(s_controls, 0, WB_MAX_CONTROLS * sizeof(wb_control_t));
    }
    memset(s_devices, 0, sizeof(s_devices));
    s_discovery_active  = true;
    s_table_full_warned = false;

    /* (Re)start the discovery close timer so every enable() gets a fresh window. */
    if (!s_discovery_timer) {
        esp_timer_create_args_t ta = { .callback = discovery_timeout_cb, .name = "wb_disc" };
        esp_timer_create(&ta, &s_discovery_timer);
    }
    if (s_discovery_timer) {
        esp_timer_stop(s_discovery_timer);
        esp_timer_start_once(s_discovery_timer, DISCOVERY_WINDOW_US);
    }

    ESP_LOGI(TAG, "Discovery enabled (%u slots × %u B = %u KB heap)",
             WB_MAX_CONTROLS,
             (unsigned)sizeof(wb_control_t),
             (unsigned)(WB_MAX_CONTROLS * sizeof(wb_control_t) / 1024));
}

void wb_driver_disable_discovery(void)
{
    s_discovery_active = false;
    int count = 0;
    if (s_controls) {
        for (int i = 0; i < WB_MAX_CONTROLS; i++)
            if (s_controls[i].flags & WB_FLAG_VALID) count++;
        free(s_controls);
        s_controls = NULL;
    }
    ESP_LOGI(TAG, "Discovery closed — %d controls found, table freed", count);
}

void wb_driver_refresh(void)
{
    wb_driver_enable_discovery();
    mqtt_client_subscribe("/devices/#", 0);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void wb_driver_init(void)
{
    memset(s_devices, 0, sizeof(s_devices));
}

void wb_driver_process(const char *topic, const char *payload, int payload_len)
{
    if (!topic) return;
    s_wb_present = true;

    /* Intercept retained /on command topics before full parsing.
     * WB controllers publish to /devices/{dev}/controls/{ctrl}/on with retain=1,
     * so these arrive during discovery.  The presence of a retained /on message
     * is definitive proof the control is writable — more reliable than type
     * inference alone (handles unrecognised type strings, late meta, etc.). */
    size_t tlen = strlen(topic);
    if (tlen > 3 && strcmp(topic + tlen - 3, "/on") == 0 &&
        s_controls && s_discovery_active) {
        /* Parse the base topic (strip trailing "/on") */
        char base[256];
        size_t base_len = tlen - 3;
        if (base_len < sizeof(base)) {
            memcpy(base, topic, base_len);
            base[base_len] = '\0';
            char dev_id2[32] = {0}, ctrl2[28] = {0}, mf2[32] = {0};
            bool im2 = false;
            if (parse_wb_topic(base, dev_id2, ctrl2, &im2, mf2)
                && ctrl2[0] && !im2 && !is_excluded(dev_id2)) {
                wb_device_t *d = find_or_add_device(dev_id2);
                if (d) {
                    uint8_t idx = (uint8_t)(d - s_devices);
                    wb_control_t *ctrl = find_or_add_control(idx, ctrl2);
                    if (ctrl) {
                        ctrl->flags &= ~WB_FLAG_READONLY;
                        ESP_LOGD(TAG, "/on retained → writable: %s/%s", dev_id2, ctrl2);
                    }
                }
            }
        }
        return;  /* /on topics carry no other useful data */
    }

    char dev_id[32]     = {0};
    char ctrl_name[28]  = {0};
    char meta_field[32] = {0};
    bool is_meta        = false;

    if (!parse_wb_topic(topic, dev_id, ctrl_name, &is_meta, meta_field))
        return;

    if (is_excluded(dev_id)) return;

    /* Look up (or add during discovery) the device entry */
    wb_device_t *dev_obj = s_discovery_active
        ? find_or_add_device(dev_id)
        : find_device(dev_id);
    if (!dev_obj) return;

    char pl[48] = {0};
    int copy = payload_len < (int)sizeof(pl) - 1 ? payload_len : (int)sizeof(pl) - 1;
    if (payload && copy > 0) memcpy(pl, payload, copy);

    /* Device-level meta (no control name) */
    if (ctrl_name[0] == '\0') {
        if (!s_discovery_active) return;
        if (strcmp(meta_field, "/name") == 0) {
            strncpy(dev_obj->name, pl, sizeof(dev_obj->name) - 1);
        } else if (meta_field[0] == '\0' || strcmp(meta_field, "/driver") == 0) {
            cJSON *j = cJSON_ParseWithLength(payload, payload_len);
            if (j) {
                cJSON *title = cJSON_GetObjectItem(j, "title");
                const char *en = NULL;
                if (cJSON_IsObject(title))
                    en = cJSON_GetStringValue(cJSON_GetObjectItem(title, "en"));
                else if (cJSON_IsString(title))
                    en = title->valuestring;
                if (en && en[0])
                    strncpy(dev_obj->name, en, sizeof(dev_obj->name) - 1);
                cJSON_Delete(j);
            }
        }
        return;
    }

    uint8_t dev_idx = (uint8_t)(dev_obj - s_devices);
    wb_control_t *ctrl = find_or_add_control(dev_idx, ctrl_name);
    if (!ctrl) return;

    if (!is_meta) {
        /* value not stored — discovery exposes names and metadata only */
    } else {
        if (meta_field[0] == '\0') {
            cJSON *j = cJSON_ParseWithLength(payload, payload_len);
            if (j) {
                const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(j, "type"));
                if (type) apply_type(ctrl, wb_type_from_string(type));

                /* explicit readonly always overrides the type-based guess */
                cJSON *ro = cJSON_GetObjectItem(j, "readonly");
                if (cJSON_IsBool(ro)) {
                    if (cJSON_IsTrue(ro)) ctrl->flags |=  WB_FLAG_READONLY;
                    else                  ctrl->flags &= ~WB_FLAG_READONLY;
                } else if (cJSON_IsNumber(ro)) {
                    if (ro->valuedouble != 0) ctrl->flags |=  WB_FLAG_READONLY;
                    else                      ctrl->flags &= ~WB_FLAG_READONLY;
                }

                cJSON *ord = cJSON_GetObjectItem(j, "order");
                if (cJSON_IsNumber(ord)) ctrl->order = (uint8_t)(ord->valuedouble);

                const char *units = cJSON_GetStringValue(cJSON_GetObjectItem(j, "units"));
                if (units) strncpy(ctrl->units, units, sizeof(ctrl->units) - 1);

                cJSON_Delete(j);
            }
        } else if (strcmp(meta_field, "/type") == 0) {
            apply_type(ctrl, wb_type_from_string(pl));
        } else if (strcmp(meta_field, "/readonly") == 0) {
            /* explicit flat topic always wins over type-based guess */
            if (pl[0] == '1') ctrl->flags |=  WB_FLAG_READONLY;
            else               ctrl->flags &= ~WB_FLAG_READONLY;
        } else if (strcmp(meta_field, "/order") == 0) {
            ctrl->order = (uint8_t)atoi(pl);
        } else if (strcmp(meta_field, "/units") == 0) {
            strncpy(ctrl->units, pl, sizeof(ctrl->units) - 1);
        }
    }
}

void wb_driver_subscribe(void)
{
    mqtt_client_subscribe("/devices/#", 0);
}

void wb_driver_set_control(const char *device_id, const char *control_name, const char *value)
{
    char topic[128];
    snprintf(topic, sizeof(topic), "/devices/%s/controls/%s/on", device_id, control_name);
    mqtt_client_publish(topic, value, 1, 0);
    ESP_LOGI(TAG, "Set %s = %s", topic, value);
}

bool wb_driver_has_controls(void) { return s_controls != NULL; }

int wb_driver_get_device_count(void)
{
    int count = 0;
    for (int i = 0; i < WB_MAX_DEVICES; i++)
        if (s_devices[i].valid) count++;
    return count;
}

const wb_device_t *wb_driver_get_device(int idx)
{
    int seen = 0;
    for (int i = 0; i < WB_MAX_DEVICES; i++) {
        if (s_devices[i].valid) {
            if (seen == idx) return &s_devices[i];
            seen++;
        }
    }
    return NULL;
}

int wb_driver_get_controls(const char *device_id, const wb_control_t **out, int max)
{
    if (!s_controls) return 0;

    /* resolve device_id to its index */
    int dev_idx = -1;
    for (int i = 0; i < WB_MAX_DEVICES; i++) {
        if (s_devices[i].valid && strcmp(s_devices[i].id, device_id) == 0) {
            dev_idx = i;
            break;
        }
    }
    if (dev_idx < 0) return 0;

    int count = 0;
    for (int i = 0; i < WB_MAX_CONTROLS && count < max; i++) {
        if ((s_controls[i].flags & WB_FLAG_VALID) &&
            s_controls[i].dev_idx == (uint8_t)dev_idx)
            out[count++] = &s_controls[i];
    }

    /* insertion sort by order */
    for (int i = 1; i < count; i++) {
        const wb_control_t *key = out[i];
        int j = i - 1;
        while (j >= 0 && out[j]->order > key->order) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }
    return count;
}
