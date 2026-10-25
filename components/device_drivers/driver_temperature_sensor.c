#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_zigbee_core.h"
#include "esp_zigbee_endpoint.h"
#include "device_driver.h"
#include "mqtt_client_wrapper.h"
#include "zigbee_coordinator.h"

static const char *TAG = "DRIVER_TEMP";

typedef struct {
    uint8_t endpoint_id;
    float temperature;
} temperature_data_t;

static esp_err_t temp_init_endpoint(uint8_t endpoint_id, const void *config)
{
    ESP_LOGI(TAG, "Initializing Temperature sensor endpoint %d", endpoint_id);
    zigbee_coordinator_add_temperature_endpoint(endpoint_id);
    return ESP_OK;
}

static esp_err_t temp_handle_mqtt(const char *payload, void *device_data)
{
    temperature_data_t *data = (temperature_data_t*)device_data;
    if (!data) {
        data = malloc(sizeof(temperature_data_t));
        memset(data, 0, sizeof(temperature_data_t));
    }

    data->temperature = atof(payload);
    ESP_LOGI(TAG, "Temperature updated: %.2f C", data->temperature);
    return ESP_OK;
}

static esp_err_t temp_handle_zigbee(void *zcl_cmd, void *device_data)
{
    temperature_data_t *data = (temperature_data_t*)device_data;
    if (!data) return ESP_FAIL;

    esp_zb_zcl_cmd_info_t *cmd = (esp_zb_zcl_cmd_info_t*)zcl_cmd;
    
    if (cmd->cluster == 0x0402 && cmd->command.id == 0x00) {
        // Note: Actual data parsing depends on the buffer passed to this function
        // This is a simplified version
        ESP_LOGI(TAG, "Zigbee: Temperature report received");
        
        char payload[64];
        snprintf(payload, sizeof(payload), "{\"temperature\":%.2f,\"unit\":\"C\"}", data->temperature);
        char topic[128];
        snprintf(topic, sizeof(topic), "zigbee/ep%d/temperature", data->endpoint_id);
        mqtt_client_publish(topic, payload, 0, 1);
    }

    return ESP_OK;
}

static esp_err_t temp_publish_state(void *device_data, const char *topic)
{
    temperature_data_t *data = (temperature_data_t*)device_data;
    if (!data) return ESP_FAIL;
    
    char payload[64];
    snprintf(payload, sizeof(payload), "{\"temperature\":%.2f,\"unit\":\"C\"}", data->temperature);
    
    mqtt_client_publish(topic, payload, 0, 1);
    return ESP_OK;
}

static size_t temp_get_config_size(void)
{
    return sizeof(temperature_data_t);
}

static esp_err_t temp_save_config(const void *device_data, char *blob, size_t *len)
{
    *len = sizeof(temperature_data_t);
    memcpy(blob, device_data, *len);
    return ESP_OK;
}

static esp_err_t temp_load_config(void *device_data, const char *blob, size_t len)
{
    memcpy(device_data, blob, len);
    return ESP_OK;
}

static void temp_free_data(void *device_data)
{
    if (device_data) free(device_data);
}

static const device_driver_interface_t s_temp_driver = {
    .device_type = "temperature_sensor",
    .display_name = "Temperature Sensor",
    .description = "Temperature sensor reporting in Celsius",
    .readonly = true,
    .properties = "[]",
    .init_endpoint = temp_init_endpoint,
    .handle_mqtt = temp_handle_mqtt,
    .handle_zigbee = temp_handle_zigbee,
    .publish_state = temp_publish_state,
    .get_config_size = temp_get_config_size,
    .save_config = temp_save_config,
    .load_config = temp_load_config,
    .free_data = temp_free_data,
};

void driver_temperature_sensor_register(void)
{
    device_driver_register(&s_temp_driver);
}
