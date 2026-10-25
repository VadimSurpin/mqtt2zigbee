#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "device_manager.h"
#include "device_driver.h"
#include "app_config.h"
#include "mqtt_client_wrapper.h"
#include "zigbee_coordinator.h"

static const char *TAG = "DEVICE_MANAGER";

#define MAX_DEVICES 128

typedef struct {
    uint8_t endpoint_id;
    char device_type[32];
    char topic[128];
    char alias[64];
    char mfr_name[33];
    char model_id[33];
    char zb_subtype[16];  /* "light" | "switch" — passed to init_endpoint as config */
    void *device_data;
    const device_driver_t *driver;
    bool active;
} device_entry_t;

static device_entry_t s_devices[MAX_DEVICES];
static device_event_handler_t s_event_handler = NULL;
static uint8_t s_next_endpoint_id = 11;
static bool s_devices_restored = false;

/* Zigbee2MQTT entries use the "z2m" driver and the topic "<base>/<friendly_name>#<property>":
 * MQTT traffic is on the part before '#', and several entries (properties) can share it. */
static bool is_z2m(const device_entry_t *d)
{
    return strcmp(d->device_type, "z2m") == 0;
}

static size_t z2m_base_len(const char *topic)
{
    const char *hash = strchr(topic, '#');
    return hash ? (size_t)(hash - topic) : strlen(topic);
}

esp_err_t device_manager_init(void)
{
    memset(s_devices, 0, sizeof(s_devices));
    ESP_LOGI(TAG, "Device manager initialized");
    return ESP_OK;
}

/* "/devices/<dev>/controls/<ctrl>" → "<dev>/<ctrl>" (≤32 chars, truncated) */
static void topic_to_default_model(const char *topic, char *out, size_t out_sz)
{
    const char *d = strstr(topic, "/devices/");
    if (d) d += 9;
    else { d = strstr(topic, "devices/"); if (d) d += 8; else d = topic; }
    const char *c = strstr(d, "/controls/");
    if (c)
        snprintf(out, out_sz, "%.*s/%s", (int)(c - d), d, c + 10);
    else
        snprintf(out, out_sz, "%s", d);
}

static const char* guess_device_type(const char *topic)
{
    if (strncmp(topic, "/devices/", 9) == 0 || strncmp(topic, "devices/", 8) == 0)
        return "wb_switch";   /* default WB fallback; explicit type preferred */
    if (strstr(topic, "temp"))                                    return "temperature_sensor";
    if (strstr(topic, "humid"))                                   return "humidity_sensor";
    if (strstr(topic, "pressure") || strstr(topic, "press"))      return "pressure_sensor";
    if (strstr(topic, "contact") || strstr(topic, "door") || strstr(topic, "window"))
                                                                  return "contact_sensor";
    if (strstr(topic, "rgb") || strstr(topic, "color"))           return "rgb_light";
    return "onoff_switch";
}

esp_err_t device_manager_add_device(uint8_t endpoint_id, const char *device_type, const char *topic, const char *alias, const char *mfr_name, const char *model_id, const char *zb_subtype)
{
    const device_driver_t *driver = device_driver_find(device_type);
    if (!driver) {
        ESP_LOGE(TAG, "Driver not found for type: %s", device_type);
        return ESP_ERR_NOT_FOUND;
    }

    int slot = -1;
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!s_devices[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return ESP_ERR_NO_MEM;

    device_entry_t *dev = &s_devices[slot];
    dev->endpoint_id = endpoint_id > 0 ? endpoint_id : s_next_endpoint_id++;
    strncpy(dev->device_type, device_type, sizeof(dev->device_type) - 1);
    strncpy(dev->topic, topic, sizeof(dev->topic) - 1);
    strncpy(dev->alias, alias ? alias : "", sizeof(dev->alias) - 1);

    strncpy(dev->mfr_name, (mfr_name && mfr_name[0]) ? mfr_name : "WB",
            sizeof(dev->mfr_name) - 1);
    if (model_id && model_id[0]) {
        strncpy(dev->model_id, model_id, sizeof(dev->model_id) - 1);
    } else {
        topic_to_default_model(topic, dev->model_id, sizeof(dev->model_id));
    }
    strncpy(dev->zb_subtype, (zb_subtype && zb_subtype[0]) ? zb_subtype : "",
            sizeof(dev->zb_subtype) - 1);

    dev->driver = driver;
    dev->device_data = NULL;
    dev->active = true;

    if (driver->iface->get_config_size) {
        size_t sz = driver->iface->get_config_size();
        if (sz > 0) {
            dev->device_data = calloc(1, sz);
            if (!dev->device_data) {
                dev->active = false;
                return ESP_ERR_NO_MEM;
            }
            // Set endpoint_id as first byte; works for onoff/rgb/temperature which
            // have uint8_t endpoint_id as their first struct field.
            *(uint8_t*)dev->device_data = dev->endpoint_id;
        }
    }

    if (driver->iface->set_topic && dev->device_data) {
        driver->iface->set_topic(dev->device_data, dev->topic);
    }

    if (is_z2m(dev)) {
        /* Z2M publishes the whole device state as JSON on the base topic. */
        char base[128];
        size_t n = z2m_base_len(dev->topic);
        memcpy(base, dev->topic, n);
        base[n] = '\0';
        /* One wildcard for all devices keeps within the MQTT wrapper's small subscription
           table; a friendly_name containing '/' is not matched by '+', so subscribe it exactly. */
        char *slash = strchr(base, '/');
        if (slash && !strchr(slash + 1, '/')) {
            strcpy(slash + 1, "+");
        }
        mqtt_client_subscribe(base, 0);
    } else
    // Subscribe to MQTT command topic for writable (actuator) devices
    if (!driver->iface->readonly) {
        char set_topic[160];
        snprintf(set_topic, sizeof(set_topic), "%s/set", topic);
        mqtt_client_subscribe(set_topic, 1);
    }

    ESP_LOGI(TAG, "Added device: %s at endpoint %d, topic: %s", device_type, dev->endpoint_id, topic);
    return ESP_OK;
}

esp_err_t device_manager_remove_device(uint8_t endpoint_id)
{
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (s_devices[i].active && s_devices[i].endpoint_id == endpoint_id) {
            if (s_devices[i].driver && s_devices[i].driver->iface->free_data && s_devices[i].device_data) {
                s_devices[i].driver->iface->free_data(s_devices[i].device_data);
            }
            s_devices[i].device_data = NULL;
            s_devices[i].active = false;
            ESP_LOGI(TAG, "Removed device at endpoint %d", endpoint_id);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t device_manager_get_all_devices(char *json_buffer, size_t buffer_size)
{
    int offset = 0;
    offset += snprintf(json_buffer + offset, buffer_size - offset, "[");

    for (int i = 0; i < MAX_DEVICES; i++) {
        if (s_devices[i].active) {
            if (offset > 1) offset += snprintf(json_buffer + offset, buffer_size - offset, ",");
            offset += snprintf(json_buffer + offset, buffer_size - offset,
                "{\"endpoint_id\":%d,\"device_type\":\"%s\",\"topic\":\"%s\",\"alias\":\"%s\"}",
                s_devices[i].endpoint_id, s_devices[i].device_type, s_devices[i].topic, s_devices[i].alias);
        }
    }
    offset += snprintf(json_buffer + offset, buffer_size - offset, "]");
    return ESP_OK;
}

const device_driver_t* device_manager_get_driver(const char *device_type)
{
    return device_driver_find(device_type);
}

esp_err_t device_manager_handle_mqtt_message(const char *topic, const char *payload)
{
    bool handled = false;
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!s_devices[i].active) continue;

        if (is_z2m(&s_devices[i])) {
            /* Several properties of one device share the topic: notify every match. */
            size_t n = z2m_base_len(s_devices[i].topic);
            if (strlen(topic) == n && strncmp(topic, s_devices[i].topic, n) == 0 &&
                s_devices[i].driver && s_devices[i].driver->iface->handle_mqtt) {
                s_devices[i].driver->iface->handle_mqtt(payload, s_devices[i].device_data);
                handled = true;
            }
            continue;
        }

        char full_topic[160] = {0};
        snprintf(full_topic, sizeof(full_topic), "%s/set", s_devices[i].topic);

        if (strcmp(topic, s_devices[i].topic) == 0 || strcmp(topic, full_topic) == 0) {
            if (s_devices[i].driver && s_devices[i].driver->iface->handle_mqtt) {
                s_devices[i].driver->iface->handle_mqtt(payload, s_devices[i].device_data);
                return ESP_OK;
            }
        }
    }
    return handled ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t device_manager_handle_zigbee_attribute(uint8_t endpoint_id, uint16_t cluster,
                                                   uint16_t attr_id, const void *value,
                                                   uint16_t size, uint8_t data_type)
{
    device_entry_t *dev = NULL;
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (s_devices[i].active && s_devices[i].endpoint_id == endpoint_id) {
            dev = &s_devices[i];
            break;
        }
    }
    if (!dev) {
        ESP_LOGD(TAG, "No device for endpoint %d", endpoint_id);
        return ESP_ERR_NOT_FOUND;
    }
    if (!value) return ESP_ERR_INVALID_ARG;

    if (is_z2m(dev)) {
        /* Zigbee→Z2M: only On/Off is writable; command is JSON on <base>/set. */
        if (cluster != 0x0006 || attr_id != 0x0000) return ESP_OK;
        const char *hash = strchr(dev->topic, '#');
        const char *prop = (hash && hash[1]) ? hash + 1 : "state";
        char set_topic[160], body[64];
        snprintf(set_topic, sizeof(set_topic), "%.*s/set", (int)z2m_base_len(dev->topic), dev->topic);
        snprintf(body, sizeof(body), "{\"%s\":\"%s\"}", prop, *(const uint8_t *)value ? "ON" : "OFF");
        mqtt_client_publish(set_topic, body, 0, 0);
        ESP_LOGI(TAG, "Zigbee→Z2M ep=%d: %s %s", endpoint_id, set_topic, body);
        return ESP_OK;
    }

    char payload[128] = {0};
    char topic[160];
    /* WB command topic: {control}/on with plain value (not JSON) */
    snprintf(topic, sizeof(topic), "%s/on", dev->topic);

    switch (cluster) {
    case 0x0006:  // On/Off → WB /on with "1" or "0"
        if (attr_id == 0x0000) {
            uint8_t on = *(uint8_t*)value;
            snprintf(payload, sizeof(payload), "%d", on ? 1 : 0);
        }
        break;
    case 0x0008:  // Level Control — CurrentLevel (0–254) → WB /on with 0–100
        if (attr_id == 0x0000) {
            uint8_t level = *(uint8_t*)value;
            snprintf(payload, sizeof(payload), "%.1f", level * 100.0f / 254.0f);
        }
        break;
    /* Sensor clusters are read-only push-from-device; Alice does not write them. */
    case 0x0402:
    case 0x0405:
    case 0x0403:
    case 0x0500:
        return ESP_OK;
    default:
        ESP_LOGD(TAG, "Unhandled cluster 0x%04x attr 0x%04x on ep %d", cluster, attr_id, endpoint_id);
        return ESP_OK;
    }

    if (payload[0]) {
        mqtt_client_publish(topic, payload, 0, 1);
        ESP_LOGI(TAG, "Zigbee→MQTT ep=%d: %s = %s", endpoint_id, topic, payload);
    }
    return ESP_OK;
}

esp_err_t device_manager_publish_state(uint8_t endpoint_id)
{
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (s_devices[i].active && s_devices[i].endpoint_id == endpoint_id) {
            if (s_devices[i].driver && s_devices[i].driver->iface->publish_state) {
                return s_devices[i].driver->iface->publish_state(s_devices[i].device_data, s_devices[i].topic);
            }
        }
    }
    return ESP_ERR_NOT_FOUND;
}

void device_manager_set_event_handler(device_event_handler_t handler)
{
    s_event_handler = handler;
}

esp_err_t device_manager_restore_from_nvs(void)
{
    if (s_devices_restored) return ESP_OK;
    s_devices_restored = true;

    const size_t bufsize = 4096;
    char *devices_json = calloc(1, bufsize);
    if (!devices_json) return ESP_ERR_NO_MEM;
    size_t len = bufsize - 1;

    esp_err_t err = app_config_load_devices(devices_json, &len);
    if (err != ESP_OK || len == 0) {
        ESP_LOGI(TAG, "No devices in NVS to restore");
        free(devices_json);
        return ESP_OK;
    }
    devices_json[len] = '\0';

    ESP_LOGI(TAG, "Restoring devices from NVS");

    cJSON *root = cJSON_Parse(devices_json);
    free(devices_json);
    if (!root || !cJSON_IsArray(root)) {
        if (root) cJSON_Delete(root);
        ESP_LOGE(TAG, "Failed to parse devices JSON");
        return ESP_FAIL;
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        if (cJSON_IsString(item)) {
            const char *raw = item->valuestring;
            if (!raw || !raw[0]) continue;
            /* Format: "type|topic[|mfr[|model]]" */
            const char *p1 = strchr(raw, '|');
            if (!p1 || p1 == raw || (p1 - raw) >= 32) {
                /* No type prefix — treat whole string as topic */
                device_manager_add_device(0, guess_device_type(raw), raw, "", NULL, NULL, NULL);
                continue;
            }
            char dtype[32] = {0};
            strncpy(dtype, raw, (size_t)(p1 - raw));

            const char *p2 = strchr(p1 + 1, '|');
            if (!p2) {
                /* type|topic only */
                device_manager_add_device(0, dtype, p1 + 1, "", NULL, NULL, NULL);
                continue;
            }
            char topic_buf[128] = {0};
            strncpy(topic_buf, p1 + 1, (size_t)(p2 - p1 - 1) < 127 ? (size_t)(p2 - p1 - 1) : 127);

            const char *p3 = strchr(p2 + 1, '|');
            char mfr[33] = {0}, model[33] = {0}, subtype[16] = {0};
            if (!p3) {
                /* type|topic|mfr */
                strncpy(mfr, p2 + 1, 32);
            } else {
                /* type|topic|mfr|model[|subtype] */
                strncpy(mfr, p2 + 1, (size_t)(p3 - p2 - 1) < 32 ? (size_t)(p3 - p2 - 1) : 32);
                const char *p4 = strchr(p3 + 1, '|');
                if (!p4) {
                    strncpy(model, p3 + 1, 32);
                } else {
                    strncpy(model, p3 + 1, (size_t)(p4 - p3 - 1) < 32 ? (size_t)(p4 - p3 - 1) : 32);
                    strncpy(subtype, p4 + 1, 15);
                }
            }
            device_manager_add_device(0, dtype, topic_buf, "", mfr, model, subtype);

        } else if (cJSON_IsObject(item)) {
            /* Legacy format: {device_type, topic, alias} objects */
            cJSON *dtype  = cJSON_GetObjectItem(item, "device_type");
            cJSON *topic  = cJSON_GetObjectItem(item, "topic");
            cJSON *alias  = cJSON_GetObjectItem(item, "alias");
            if (topic && cJSON_IsString(topic)) {
                const char *dt = (dtype && cJSON_IsString(dtype)) ? dtype->valuestring : "onoff_switch";
                const char *al = (alias && cJSON_IsString(alias)) ? alias->valuestring : "";
                device_manager_add_device(0, dt, topic->valuestring, al, NULL, NULL, NULL);
            }
        }
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Devices restored from NVS");
    return ESP_OK;
}

void device_manager_build_zigbee_endpoints(void)
{
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!s_devices[i].active) continue;
        const device_driver_t *drv = s_devices[i].driver;
        if (drv && drv->iface->init_endpoint) {
            /* Prefer alias as ZCL ModelIdentifier so Alice shows a readable name. */
            const char *model = s_devices[i].alias[0] ? s_devices[i].alias
                                                       : s_devices[i].model_id;
            zigbee_coordinator_set_basic_info(s_devices[i].mfr_name, model);
            const char *sub = s_devices[i].zb_subtype[0] ? s_devices[i].zb_subtype : NULL;
            drv->iface->init_endpoint(s_devices[i].endpoint_id, sub);
        }
    }
}

esp_err_t device_manager_save_to_nvs(void)
{
    cJSON *root = cJSON_CreateArray();

    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!s_devices[i].active) continue;

        cJSON *device = cJSON_CreateObject();
        cJSON_AddNumberToObject(device, "endpoint_id", s_devices[i].endpoint_id);
        cJSON_AddStringToObject(device, "device_type", s_devices[i].device_type);
        cJSON_AddStringToObject(device, "topic", s_devices[i].topic);
        cJSON_AddStringToObject(device, "alias", s_devices[i].alias);
        cJSON_AddItemToArray(root, device);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_FAIL;

    esp_err_t err = app_config_save_devices(json, strlen(json));
    free(json);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Devices saved to NVS");
    }
    return err;
}
