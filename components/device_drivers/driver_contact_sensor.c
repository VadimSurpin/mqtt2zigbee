#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "device_driver.h"
#include "mqtt_client_wrapper.h"
#include "zigbee_coordinator.h"

static const char *TAG = "DRIVER_CONTACT";

typedef struct { bool contact_open; } contact_data_t;

static esp_err_t contact_init_endpoint(uint8_t endpoint_id, const void *config) {
    ESP_LOGI(TAG, "Initializing Contact sensor endpoint %d", endpoint_id);
    zigbee_coordinator_add_contact_endpoint(endpoint_id);
    return ESP_OK;
}
static esp_err_t contact_handle_mqtt(const char *payload, void *device_data) {
    contact_data_t *data = (contact_data_t*)device_data;
    if (!data) { data = malloc(sizeof(contact_data_t)); memset(data, 0, sizeof(contact_data_t)); }
    if (strstr(payload, "open") || strstr(payload, "OPEN") || strstr(payload, "true")) data->contact_open = true;
    else if (strstr(payload, "closed") || strstr(payload, "CLOSED") || strstr(payload, "false")) data->contact_open = false;
    ESP_LOGI(TAG, "Contact: %s", data->contact_open ? "open" : "closed");
    return ESP_OK;
}
static esp_err_t contact_publish_state(void *device_data, const char *topic) {
    contact_data_t *data = (contact_data_t*)device_data;
    char payload[64];
    snprintf(payload, sizeof(payload), "{\"contact\":\"%s\"}", data->contact_open ? "open" : "closed");
    mqtt_client_publish(topic, payload, 0, 1);
    return ESP_OK;
}
static size_t contact_get_config_size(void) { return sizeof(contact_data_t); }
static esp_err_t contact_save_config(const void *d, char *b, size_t *l) { *l = sizeof(contact_data_t); memcpy(b, d, *l); return ESP_OK; }
static esp_err_t contact_load_config(void *d, const char *b, size_t l) { memcpy(d, b, l); return ESP_OK; }
static void contact_free_data(void *d) { if (d) free(d); }

static const device_driver_interface_t s_contact_driver = {
    .device_type = "contact_sensor", .display_name = "Door/Window Contact Sensor", .description = "Contact sensor for doors and windows",
    .readonly = true, .properties = "[]",
    .init_endpoint = contact_init_endpoint, .handle_mqtt = contact_handle_mqtt, .publish_state = contact_publish_state,
    .get_config_size = contact_get_config_size, .save_config = contact_save_config, .load_config = contact_load_config, .free_data = contact_free_data,
};
void driver_contact_sensor_register(void) { device_driver_register(&s_contact_driver); }
