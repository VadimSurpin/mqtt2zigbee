#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_zigbee_core.h"
#include "esp_zigbee_endpoint.h"
#include "device_driver.h"
#include "mqtt_client_wrapper.h"
#include "zigbee_coordinator.h"

static const char *TAG = "DRIVER_ONOFF";

typedef struct {
    uint8_t endpoint_id;
    bool state;
} onoff_data_t;

static esp_err_t onoff_init_endpoint(uint8_t endpoint_id, const void *config)
{
    ESP_LOGI(TAG, "Initializing On/Off switch endpoint %d", endpoint_id);
    zigbee_coordinator_add_onoff_endpoint(endpoint_id);
    return ESP_OK;
}

static esp_err_t onoff_handle_mqtt(const char *payload, void *device_data)
{
    onoff_data_t *data = (onoff_data_t*)device_data;
    if (!data) {
        data = malloc(sizeof(onoff_data_t));
        memset(data, 0, sizeof(onoff_data_t));
    }

    if (strstr(payload, "ON") || strstr(payload, "on") || strstr(payload, "true")) {
        data->state = true;
    } else if (strstr(payload, "OFF") || strstr(payload, "off") || strstr(payload, "false")) {
        data->state = false;
    } else if (strstr(payload, "TOGGLE") || strstr(payload, "toggle")) {
        data->state = !data->state;
    }

    if (data->endpoint_id > 0) {
        zigbee_coordinator_set_onoff(data->endpoint_id, data->state);
    }
    ESP_LOGI(TAG, "On/Off state: %s", data->state ? "ON" : "OFF");
    return ESP_OK;
}

static esp_err_t onoff_handle_zigbee(void *zcl_cmd, void *device_data)
{
    onoff_data_t *data = (onoff_data_t*)device_data;
    if (!data) return ESP_FAIL;

    esp_zb_zcl_cmd_info_t *cmd = (esp_zb_zcl_cmd_info_t*)zcl_cmd;
    
    if (cmd->cluster == 0x0006) {
        switch (cmd->command.id) {
            case 0x00:
                data->state = true;
                ESP_LOGI(TAG, "Zigbee: On command");
                break;
            case 0x01:
                data->state = false;
                ESP_LOGI(TAG, "Zigbee: Off command");
                break;
            case 0x02:
                data->state = !data->state;
                ESP_LOGI(TAG, "Zigbee: Toggle command, new state: %s", data->state ? "ON" : "OFF");
                break;
            default:
                break;
        }

        char payload[32];
        snprintf(payload, sizeof(payload), "{\"state\":\"%s\"}", data->state ? "ON" : "OFF");
        char topic[128];
        snprintf(topic, sizeof(topic), "zigbee/ep%d/onoff/state", data->endpoint_id);
        mqtt_client_publish(topic, payload, 0, 1);
    }

    return ESP_OK;
}

static esp_err_t onoff_publish_state(void *device_data, const char *topic)
{
    onoff_data_t *data = (onoff_data_t*)device_data;
    if (!data) return ESP_FAIL;
    
    char payload[32];
    snprintf(payload, sizeof(payload), "{\"state\":\"%s\"}", data->state ? "ON" : "OFF");
    
    char full_topic[128];
    snprintf(full_topic, sizeof(full_topic), "%s/state", topic);
    
    mqtt_client_publish(full_topic, payload, 0, 1);
    return ESP_OK;
}

static size_t onoff_get_config_size(void)
{
    return sizeof(onoff_data_t);
}

static esp_err_t onoff_save_config(const void *device_data, char *blob, size_t *len)
{
    *len = sizeof(onoff_data_t);
    memcpy(blob, device_data, *len);
    return ESP_OK;
}

static esp_err_t onoff_load_config(void *device_data, const char *blob, size_t len)
{
    memcpy(device_data, blob, len);
    return ESP_OK;
}

static void onoff_free_data(void *device_data)
{
    if (device_data) free(device_data);
}

static const device_driver_interface_t s_onoff_driver = {
    .device_type = "onoff_switch",
    .display_name = "On/Off Switch",
    .description = "Basic on/off switch with toggle support",
    .readonly = false,
    .properties = "[{\"name\":\"on_payload\",\"type\":\"string\",\"default\":\"ON\"},{\"name\":\"off_payload\",\"type\":\"string\",\"default\":\"OFF\"},{\"name\":\"toggle_payload\",\"type\":\"string\",\"default\":\"TOGGLE\"}]",
    .init_endpoint = onoff_init_endpoint,
    .handle_mqtt = onoff_handle_mqtt,
    .handle_zigbee = onoff_handle_zigbee,
    .publish_state = onoff_publish_state,
    .get_config_size = onoff_get_config_size,
    .save_config = onoff_save_config,
    .load_config = onoff_load_config,
    .free_data = onoff_free_data,
};

void driver_onoff_switch_register(void)
{
    device_driver_register(&s_onoff_driver);
}
