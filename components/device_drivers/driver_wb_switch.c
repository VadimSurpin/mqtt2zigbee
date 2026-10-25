#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_zigbee_core.h"
#include "device_driver.h"
#include "mqtt_client_wrapper.h"
#include "zigbee_coordinator.h"  /* needed for wb_switch_init_endpoint and handle_zigbee */

static const char *TAG = "DRIVER_WB_SWITCH";

typedef struct {
    uint8_t endpoint_id;
    char topic[128];
    bool state;
} wb_switch_data_t;

static esp_err_t wb_switch_init_endpoint(uint8_t endpoint_id, const void *config)
{
    bool as_switch = (config && strcmp((const char *)config, "switch") == 0);
    ESP_LOGI(TAG, "Registering WB %s endpoint %d",
             as_switch ? "switch" : "light", endpoint_id);
    return as_switch
        ? zigbee_coordinator_add_switch_endpoint(endpoint_id)
        : zigbee_coordinator_add_onoff_endpoint(endpoint_id);
}

static void wb_switch_set_topic(void *device_data, const char *topic)
{
    wb_switch_data_t *data = (wb_switch_data_t *)device_data;
    if (data && topic)
        strncpy(data->topic, topic, sizeof(data->topic) - 1);
}

static esp_err_t wb_switch_handle_mqtt(const char *payload, void *device_data)
{
    wb_switch_data_t *data = (wb_switch_data_t *)device_data;
    if (!data || !payload) return ESP_ERR_INVALID_ARG;

    /* WB switch publishes "0" or "1" — update local state only.
     * Zigbee attribute writes must happen inside the Zigbee task; calling
     * esp_zb_zcl_set_attribute_val from the MQTT task causes a race on the
     * single-core ESP32-C6 and crashes the device. */
    bool new_state;
    if (payload[0] == '1' && payload[1] == '\0')
        new_state = true;
    else if (payload[0] == '0' && payload[1] == '\0')
        new_state = false;
    else
        return ESP_OK;

    data->state = new_state;
    ESP_LOGI(TAG, "WB switch ep=%d state=%s", data->endpoint_id, data->state ? "ON" : "OFF");
    zigbee_coordinator_set_onoff(data->endpoint_id, data->state);
    return ESP_OK;
}

static esp_err_t wb_switch_handle_zigbee(void *zcl_cmd, void *device_data)
{
    wb_switch_data_t *data = (wb_switch_data_t *)device_data;
    if (!data || !zcl_cmd) return ESP_FAIL;

    esp_zb_zcl_cmd_info_t *cmd = (esp_zb_zcl_cmd_info_t *)zcl_cmd;
    if (cmd->cluster != 0x0006) return ESP_OK;

    switch (cmd->command.id) {
    case 0x00: data->state = false;        break;  /* Off */
    case 0x01: data->state = true;         break;  /* On */
    case 0x02: data->state = !data->state; break;  /* Toggle */
    default:   return ESP_OK;
    }

    /* Write to WB: publish "1"/"0" to <topic>/on */
    char on_topic[160];
    snprintf(on_topic, sizeof(on_topic), "%s/on", data->topic);
    mqtt_client_publish(on_topic, data->state ? "1" : "0", 1, 0);
    ESP_LOGI(TAG, "Zigbee→WB ep=%d %s", data->endpoint_id, data->state ? "ON" : "OFF");
    return ESP_OK;
}

static esp_err_t wb_switch_publish_state(void *device_data, const char *topic)
{
    return ESP_OK;
}

static size_t    wb_switch_get_config_size(void) { return sizeof(wb_switch_data_t); }

static esp_err_t wb_switch_save_config(const void *d, char *blob, size_t *len)
{ *len = sizeof(wb_switch_data_t); memcpy(blob, d, *len); return ESP_OK; }

static esp_err_t wb_switch_load_config(void *d, const char *blob, size_t len)
{ memcpy(d, blob, len); return ESP_OK; }

static void wb_switch_free_data(void *d) { if (d) free(d); }

static const device_driver_interface_t s_wb_switch_driver = {
    .device_type   = "wb_switch",
    .display_name  = "WB Switch",
    .description   = "Wirenboard switch/relay exposed as Zigbee On/Off endpoint",
    .readonly      = false,
    .properties    = "[]",
    .init_endpoint = wb_switch_init_endpoint,
    .set_topic     = wb_switch_set_topic,
    .handle_mqtt   = wb_switch_handle_mqtt,
    .handle_zigbee = wb_switch_handle_zigbee,
    .publish_state = wb_switch_publish_state,
    .get_config_size = wb_switch_get_config_size,
    .save_config   = wb_switch_save_config,
    .load_config   = wb_switch_load_config,
    .free_data     = wb_switch_free_data,
};

void driver_wb_switch_register(void)
{
    device_driver_register(&s_wb_switch_driver);
}
