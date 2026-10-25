#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "device_driver.h"
#include "mqtt_client_wrapper.h"
#include "zigbee_coordinator.h"

static const char *TAG = "DRIVER_HUMIDITY";

typedef struct {
    float humidity;
} humidity_data_t;

static esp_err_t humidity_init_endpoint(uint8_t endpoint_id, const void *config)
{
    ESP_LOGI(TAG, "Initializing Humidity sensor endpoint %d", endpoint_id);
    zigbee_coordinator_add_humidity_endpoint(endpoint_id);
    return ESP_OK;
}

static esp_err_t humidity_handle_mqtt(const char *payload, void *device_data)
{
    humidity_data_t *data = (humidity_data_t*)device_data;
    if (!data) {
        data = malloc(sizeof(humidity_data_t));
        memset(data, 0, sizeof(humidity_data_t));
    }
    data->humidity = atof(payload);
    ESP_LOGI(TAG, "Humidity updated: %.2f%%", data->humidity);
    return ESP_OK;
}

static esp_err_t humidity_publish_state(void *device_data, const char *topic)
{
    humidity_data_t *data = (humidity_data_t*)device_data;
    char payload[64];
    snprintf(payload, sizeof(payload), "{\"humidity\":%.2f,\"unit\":\"percent\"}", data->humidity);
    mqtt_client_publish(topic, payload, 0, 1);
    return ESP_OK;
}

static size_t humidity_get_config_size(void) { return sizeof(humidity_data_t); }
static esp_err_t humidity_save_config(const void *d, char *b, size_t *l) { *l = sizeof(humidity_data_t); memcpy(b, d, *l); return ESP_OK; }
static esp_err_t humidity_load_config(void *d, const char *b, size_t l) { memcpy(d, b, l); return ESP_OK; }
static void humidity_free_data(void *d) { if (d) free(d); }

static const device_driver_interface_t s_humidity_driver = {
    .device_type = "humidity_sensor",
    .display_name = "Humidity Sensor",
    .description = "Relative humidity sensor in percent",
    .readonly = true,
    .properties = "[]",
    .init_endpoint = humidity_init_endpoint,
    .handle_mqtt = humidity_handle_mqtt,
    .publish_state = humidity_publish_state,
    .get_config_size = humidity_get_config_size,
    .save_config = humidity_save_config,
    .load_config = humidity_load_config,
    .free_data = humidity_free_data,
};

void driver_humidity_sensor_register(void) { device_driver_register(&s_humidity_driver); }
