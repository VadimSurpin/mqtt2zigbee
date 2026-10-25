#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "device_driver.h"
#include "mqtt_client_wrapper.h"
#include "zigbee_coordinator.h"

static const char *TAG = "DRIVER_PRESSURE";

typedef struct { float pressure; } pressure_data_t;

static esp_err_t pressure_init_endpoint(uint8_t endpoint_id, const void *config) {
    ESP_LOGI(TAG, "Initializing Pressure sensor endpoint %d", endpoint_id);
    zigbee_coordinator_add_pressure_endpoint(endpoint_id);
    return ESP_OK;
}
static esp_err_t pressure_handle_mqtt(const char *payload, void *device_data) {
    pressure_data_t *data = (pressure_data_t*)device_data;
    if (!data) { data = malloc(sizeof(pressure_data_t)); memset(data, 0, sizeof(pressure_data_t)); }
    data->pressure = atof(payload);
    ESP_LOGI(TAG, "Pressure updated: %.2f hPa", data->pressure);
    return ESP_OK;
}
static esp_err_t pressure_publish_state(void *device_data, const char *topic) {
    pressure_data_t *data = (pressure_data_t*)device_data;
    char payload[64];
    snprintf(payload, sizeof(payload), "{\"pressure\":%.2f,\"unit\":\"hPa\"}", data->pressure);
    mqtt_client_publish(topic, payload, 0, 1);
    return ESP_OK;
}
static size_t pressure_get_config_size(void) { return sizeof(pressure_data_t); }
static esp_err_t pressure_save_config(const void *d, char *b, size_t *l) { *l = sizeof(pressure_data_t); memcpy(b, d, *l); return ESP_OK; }
static esp_err_t pressure_load_config(void *d, const char *b, size_t l) { memcpy(d, b, l); return ESP_OK; }
static void pressure_free_data(void *d) { if (d) free(d); }

static const device_driver_interface_t s_pressure_driver = {
    .device_type = "pressure_sensor", .display_name = "Pressure Sensor", .description = "Barometric pressure sensor in hPa",
    .readonly = true, .properties = "[]",
    .init_endpoint = pressure_init_endpoint, .handle_mqtt = pressure_handle_mqtt, .publish_state = pressure_publish_state,
    .get_config_size = pressure_get_config_size, .save_config = pressure_save_config, .load_config = pressure_load_config, .free_data = pressure_free_data,
};
void driver_pressure_sensor_register(void) { device_driver_register(&s_pressure_driver); }
