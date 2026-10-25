#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>
#include "esp_log.h"
#include "device_driver.h"
#include "zigbee_coordinator.h"

static const char *TAG = "DRIVER_Z2M";

/* One Zigbee2MQTT property exposed as one Zigbee endpoint (same idea as a WB control).
 * The configured topic is "<base>/<friendly_name>#<property>": the base topic carries the
 * device's JSON state, the property selects the value this endpoint mirrors. */
typedef struct {
    uint8_t endpoint_id;   /* must stay first: device_manager writes it */
    char    topic[96];     /* "<base>/<friendly_name>" */
    char    prop[20];
    bool    state;
} z2m_data_t;

typedef enum { K_SWITCH, K_TEMP, K_HUM, K_PRESS, K_CONTACT, K_ANALOG } kind_t;

static kind_t kind_of_prop(const char *prop)
{
    if (strcmp(prop, "state") == 0 || strncmp(prop, "state_l", 7) == 0) return K_SWITCH;
    if (strcmp(prop, "temperature") == 0) return K_TEMP;
    if (strcmp(prop, "humidity") == 0)    return K_HUM;
    if (strcmp(prop, "pressure") == 0)    return K_PRESS;
    if (strcmp(prop, "contact") == 0)     return K_CONTACT;
    return K_ANALOG;
}

static esp_err_t z2m_init_endpoint(uint8_t endpoint_id, const void *config)
{
    const char *sub = (const char *)config;
    if (!sub) sub = "switch";
    ESP_LOGI(TAG, "Registering Z2M %s endpoint %d", sub, endpoint_id);
    if (strcmp(sub, "light") == 0)       return zigbee_coordinator_add_onoff_endpoint(endpoint_id);
    if (strcmp(sub, "temperature") == 0) return zigbee_coordinator_add_temperature_endpoint(endpoint_id);
    if (strcmp(sub, "humidity") == 0)    return zigbee_coordinator_add_humidity_endpoint(endpoint_id);
    if (strcmp(sub, "pressure") == 0)    return zigbee_coordinator_add_pressure_endpoint(endpoint_id);
    if (strcmp(sub, "contact") == 0)     return zigbee_coordinator_add_contact_endpoint(endpoint_id);
    if (strcmp(sub, "analog") == 0)      return zigbee_coordinator_add_analog_input_endpoint(endpoint_id);
    return zigbee_coordinator_add_switch_endpoint(endpoint_id);
}

static void z2m_set_topic(void *device_data, const char *topic)
{
    z2m_data_t *d = (z2m_data_t *)device_data;
    if (!d || !topic) return;
    const char *hash = strchr(topic, '#');
    size_t n = hash ? (size_t)(hash - topic) : strlen(topic);
    if (n >= sizeof(d->topic)) n = sizeof(d->topic) - 1;
    memcpy(d->topic, topic, n);
    d->topic[n] = '\0';
    if (hash) strncpy(d->prop, hash + 1, sizeof(d->prop) - 1);
}

/* Copy the value of "<key>" from a flat JSON object into out (string without quotes, or the
 * bare number/true/false/null token). Returns false when the key is absent. */
static bool json_get(const char *json, const char *key, char *out, size_t out_sz)
{
    char pat[28];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = json;
    while ((p = strstr(p, pat)) != NULL) {
        p += strlen(pat);
        while (*p == ' ') p++;
        if (*p != ':') continue;            /* it was a value, not a key */
        p++;
        while (*p == ' ') p++;
        size_t n = 0;
        if (*p == '"') {
            p++;
            while (*p && *p != '"' && n < out_sz - 1) out[n++] = *p++;
        } else {
            while (*p && *p != ',' && *p != '}' && *p != ' ' && n < out_sz - 1) out[n++] = *p++;
        }
        out[n] = '\0';
        return n > 0;
    }
    return false;
}

static esp_err_t z2m_handle_mqtt(const char *payload, void *device_data)
{
    z2m_data_t *d = (z2m_data_t *)device_data;
    if (!d || !payload || !d->prop[0]) return ESP_ERR_INVALID_ARG;

    char v[24];
    if (!json_get(payload, d->prop, v, sizeof(v))) return ESP_OK;  /* not in this report */

    switch (kind_of_prop(d->prop)) {
    case K_SWITCH:
        if (strcasecmp(v, "ON") == 0)       d->state = true;
        else if (strcasecmp(v, "OFF") == 0) d->state = false;
        else return ESP_OK;
        zigbee_coordinator_set_onoff(d->endpoint_id, d->state);
        break;
    case K_TEMP:    zigbee_coordinator_set_temperature(d->endpoint_id, strtof(v, NULL)); break;
    case K_HUM:     zigbee_coordinator_set_humidity(d->endpoint_id, strtof(v, NULL));    break;
    case K_PRESS:   zigbee_coordinator_set_pressure(d->endpoint_id, strtof(v, NULL));    break;
    case K_CONTACT: /* Z2M: contact=true means closed */
        zigbee_coordinator_set_contact(d->endpoint_id, strcmp(v, "true") != 0);
        break;
    case K_ANALOG:  zigbee_coordinator_set_analog_input(d->endpoint_id, strtof(v, NULL)); break;
    }
    ESP_LOGD(TAG, "ep=%d %s=%s", d->endpoint_id, d->prop, v);
    return ESP_OK;
}

static esp_err_t z2m_handle_zigbee(void *zcl_cmd, void *device_data)
{
    return ESP_OK;  /* Zigbee→MQTT writes go through device_manager_handle_zigbee_attribute */
}

static esp_err_t z2m_publish_state(void *device_data, const char *topic)
{
    return ESP_OK;
}

static size_t    z2m_get_config_size(void) { return sizeof(z2m_data_t); }

static esp_err_t z2m_save_config(const void *d, char *blob, size_t *len)
{ *len = sizeof(z2m_data_t); memcpy(blob, d, *len); return ESP_OK; }

static esp_err_t z2m_load_config(void *d, const char *blob, size_t len)
{ memcpy(d, blob, len); return ESP_OK; }

static void z2m_free_data(void *d) { if (d) free(d); }

static const device_driver_interface_t s_z2m_driver = {
    .device_type   = "z2m",
    .display_name  = "Zigbee2MQTT",
    .description   = "Zigbee2MQTT device property exposed as a Zigbee endpoint",
    .readonly      = false,
    .properties    = "[]",
    .init_endpoint = z2m_init_endpoint,
    .set_topic     = z2m_set_topic,
    .handle_mqtt   = z2m_handle_mqtt,
    .handle_zigbee = z2m_handle_zigbee,
    .publish_state = z2m_publish_state,
    .get_config_size = z2m_get_config_size,
    .save_config   = z2m_save_config,
    .load_config   = z2m_load_config,
    .free_data     = z2m_free_data,
};

void driver_z2m_register(void)
{
    device_driver_register(&s_z2m_driver);
}
