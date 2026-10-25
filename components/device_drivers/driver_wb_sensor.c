#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_zigbee_core.h"
#include "device_driver.h"
#include "mqtt_client_wrapper.h"
#include "zigbee_coordinator.h"

static const char *TAG = "DRIVER_WB_SENSOR";

typedef struct {
    uint8_t endpoint_id;
    char topic[128];
    float value;
} wb_sensor_data_t;

static esp_err_t wb_sensor_init_endpoint(uint8_t endpoint_id, const void *config)
{
    ESP_LOGI(TAG, "Registering WB sensor Analog Input endpoint %d", endpoint_id);
    return zigbee_coordinator_add_analog_input_endpoint(endpoint_id);
}

static void wb_sensor_set_topic(void *device_data, const char *topic)
{
    wb_sensor_data_t *data = (wb_sensor_data_t *)device_data;
    if (data && topic)
        strncpy(data->topic, topic, sizeof(data->topic) - 1);
}

static esp_err_t wb_sensor_handle_mqtt(const char *payload, void *device_data)
{
    wb_sensor_data_t *data = (wb_sensor_data_t *)device_data;
    if (!data || !payload) return ESP_ERR_INVALID_ARG;

    data->value = atof(payload);
    ESP_LOGI(TAG, "WB sensor ep=%d value=%.4f", data->endpoint_id, data->value);
    zigbee_coordinator_set_analog_input(data->endpoint_id, data->value);
    return ESP_OK;
}

static esp_err_t wb_sensor_handle_zigbee(void *zcl_cmd, void *device_data)
{
    return ESP_OK; /* read-only */
}

static esp_err_t wb_sensor_publish_state(void *device_data, const char *topic)
{
    return ESP_OK;
}

static size_t    wb_sensor_get_config_size(void) { return sizeof(wb_sensor_data_t); }

static esp_err_t wb_sensor_save_config(const void *d, char *blob, size_t *len)
{ *len = sizeof(wb_sensor_data_t); memcpy(blob, d, *len); return ESP_OK; }

static esp_err_t wb_sensor_load_config(void *d, const char *blob, size_t len)
{ memcpy(d, blob, len); return ESP_OK; }

static void wb_sensor_free_data(void *d) { if (d) free(d); }

static const device_driver_interface_t s_wb_sensor_driver = {
    .device_type   = "wb_sensor",
    .display_name  = "WB Sensor",
    .description   = "Wirenboard numeric sensor exposed as Zigbee temperature endpoint",
    .readonly      = true,
    .properties    = "[]",
    .init_endpoint = wb_sensor_init_endpoint,
    .set_topic     = wb_sensor_set_topic,
    .handle_mqtt   = wb_sensor_handle_mqtt,
    .handle_zigbee = wb_sensor_handle_zigbee,
    .publish_state = wb_sensor_publish_state,
    .get_config_size = wb_sensor_get_config_size,
    .save_config   = wb_sensor_save_config,
    .load_config   = wb_sensor_load_config,
    .free_data     = wb_sensor_free_data,
};

void driver_wb_sensor_register(void)
{
    device_driver_register(&s_wb_sensor_driver);
}
