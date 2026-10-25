#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "device_driver.h"
#include "mqtt_client_wrapper.h"
#include "zigbee_coordinator.h"

static const char *TAG = "DRIVER_RGB";

typedef struct {
    uint8_t endpoint_id;
    bool on;
    uint8_t brightness;
    uint8_t r, g, b;
    float x, y;
} rgb_light_data_t;

static esp_err_t rgb_init_endpoint(uint8_t endpoint_id, const void *config) {
    ESP_LOGI(TAG, "Initializing RGB Light endpoint %d", endpoint_id);
    zigbee_coordinator_add_rgb_light_endpoint(endpoint_id);
    return ESP_OK;
}

static esp_err_t rgb_handle_mqtt(const char *payload, void *device_data) {
    rgb_light_data_t *data = (rgb_light_data_t*)device_data;
    if (!data) { data = malloc(sizeof(rgb_light_data_t)); memset(data, 0, sizeof(rgb_light_data_t)); }

    if (strstr(payload, "ON") || strstr(payload, "on") || strstr(payload, "true")) data->on = true;
    else if (strstr(payload, "OFF") || strstr(payload, "off") || strstr(payload, "false")) data->on = false;

    char *brightness_str = strstr(payload, "\"brightness\"");
    if (brightness_str) {
        sscanf(brightness_str + 11, "%hhu", &data->brightness);
    }

    char *color_str = strstr(payload, "\"color\"");
    if (color_str) {
        char *r_str = strstr(color_str, "\"r\"");
        char *g_str = strstr(color_str, "\"g\"");
        char *b_str = strstr(color_str, "\"b\"");
        if (r_str && g_str && b_str) {
            sscanf(r_str + 3, "%hhu", &data->r);
            sscanf(g_str + 3, "%hhu", &data->g);
            sscanf(b_str + 3, "%hhu", &data->b);
        }
    }

    ESP_LOGI(TAG, "RGB Light: on=%d, brightness=%d, r=%d g=%d b=%d", data->on, data->brightness, data->r, data->g, data->b);
    zigbee_coordinator_set_rgb_light(data->endpoint_id, data->on, data->brightness, data->r, data->g, data->b);
    return ESP_OK;
}

static esp_err_t rgb_publish_state(void *device_data, const char *topic) {
    rgb_light_data_t *data = (rgb_light_data_t*)device_data;
    char payload[128];
    snprintf(payload, sizeof(payload), 
        "{\"state\":\"%s\",\"brightness\":%d,\"color\":{\"r\":%d,\"g\":%d,\"b\":%d}}",
        data->on ? "ON" : "OFF", data->brightness, data->r, data->g, data->b);
    mqtt_client_publish(topic, payload, 0, 1);
    return ESP_OK;
}

static size_t rgb_get_config_size(void) { return sizeof(rgb_light_data_t); }
static esp_err_t rgb_save_config(const void *d, char *b, size_t *l) { *l = sizeof(rgb_light_data_t); memcpy(b, d, *l); return ESP_OK; }
static esp_err_t rgb_load_config(void *d, const char *b, size_t l) { memcpy(d, b, l); return ESP_OK; }
static void rgb_free_data(void *d) { if (d) free(d); }

static const device_driver_interface_t s_rgb_driver = {
    .device_type = "rgb_light",
    .display_name = "RGB Light",
    .description = "Dimmable color light with RGB control",
    .readonly = false,
    .properties = "[{\"name\":\"on_payload\",\"type\":\"string\",\"default\":\"ON\"},{\"name\":\"off_payload\",\"type\":\"string\",\"default\":\"OFF\"},{\"name\":\"brightness_payload\",\"type\":\"string\",\"default\":\"bri\",\"description\":\"Brightness topic suffix\"}]",
    .init_endpoint = rgb_init_endpoint,
    .handle_mqtt = rgb_handle_mqtt,
    .publish_state = rgb_publish_state,
    .get_config_size = rgb_get_config_size,
    .save_config = rgb_save_config,
    .load_config = rgb_load_config,
    .free_data = rgb_free_data,
};

void driver_rgb_light_register(void) { device_driver_register(&s_rgb_driver); }
