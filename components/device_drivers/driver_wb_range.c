#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "device_driver.h"
#include "zigbee_coordinator.h"

static const char *TAG = "DRIVER_WB_RANGE";

typedef struct {
    uint8_t endpoint_id;
    char    topic[128];
    float   value;   /* raw WB value */
    uint8_t level;   /* 0–254, last Zigbee level */
} wb_range_data_t;

/* WB range values are unbounded; scale assuming 0–100 domain (most common for
   WB dimmers / blinds / thermostats).  Clamp to 0–254 for Zigbee Level. */
static uint8_t value_to_level(float v)
{
    if (v <= 0.0f)   return 0;
    if (v >= 100.0f) return 254;
    return (uint8_t)(v * 254.0f / 100.0f + 0.5f);
}


static esp_err_t wb_range_init_endpoint(uint8_t endpoint_id, const void *config)
{
    ESP_LOGI(TAG, "Registering WB range Level Control endpoint %d", endpoint_id);
    return zigbee_coordinator_add_level_endpoint(endpoint_id);
}

static void wb_range_set_topic(void *device_data, const char *topic)
{
    wb_range_data_t *data = (wb_range_data_t *)device_data;
    if (data && topic)
        strncpy(data->topic, topic, sizeof(data->topic) - 1);
}

static esp_err_t wb_range_handle_mqtt(const char *payload, void *device_data)
{
    wb_range_data_t *data = (wb_range_data_t *)device_data;
    if (!data || !payload) return ESP_ERR_INVALID_ARG;

    data->value = atof(payload);
    data->level = value_to_level(data->value);
    ESP_LOGI(TAG, "WB range ep=%d value=%.2f → level=%u", data->endpoint_id, data->value, data->level);
    zigbee_coordinator_set_level(data->endpoint_id, data->level);
    return ESP_OK;
}

static esp_err_t wb_range_handle_zigbee(void *zcl_cmd, void *device_data)
{
    /* Level attribute changes are handled in device_manager_handle_zigbee_attribute
     * for cluster 0x0008 attr 0x0000 (CurrentLevel). Nothing to do here. */
    return ESP_OK;
}

static esp_err_t wb_range_publish_state(void *device_data, const char *topic)
{
    return ESP_OK;
}

static size_t    wb_range_get_config_size(void) { return sizeof(wb_range_data_t); }

static esp_err_t wb_range_save_config(const void *d, char *blob, size_t *len)
{ *len = sizeof(wb_range_data_t); memcpy(blob, d, *len); return ESP_OK; }

static esp_err_t wb_range_load_config(void *d, const char *blob, size_t len)
{ memcpy(d, blob, len); return ESP_OK; }

static void wb_range_free_data(void *d) { if (d) free(d); }

static const device_driver_interface_t s_wb_range_driver = {
    .device_type   = "wb_range",
    .display_name  = "WB Range",
    .description   = "Wirenboard range/dimmer exposed as Zigbee Level Control endpoint",
    .readonly      = false,
    .properties    = "[]",
    .init_endpoint = wb_range_init_endpoint,
    .set_topic     = wb_range_set_topic,
    .handle_mqtt   = wb_range_handle_mqtt,
    .handle_zigbee = wb_range_handle_zigbee,
    .publish_state = wb_range_publish_state,
    .get_config_size = wb_range_get_config_size,
    .save_config   = wb_range_save_config,
    .load_config   = wb_range_load_config,
    .free_data     = wb_range_free_data,
};

void driver_wb_range_register(void)
{
    device_driver_register(&s_wb_range_driver);
}
