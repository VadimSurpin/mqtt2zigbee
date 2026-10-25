#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "device_driver.h"

static const char *TAG = "DRIVER_WB_TEXT";

typedef struct {
    uint8_t endpoint_id;
    char    topic[128];
    char    value[64];
} wb_text_data_t;

static esp_err_t wb_text_init_endpoint(uint8_t endpoint_id, const void *config)
{
    /* Text controls have no ZCL equivalent — MQTT bridging only, no Zigbee endpoint. */
    ESP_LOGI(TAG, "WB text ep=%d: no Zigbee endpoint (text type)", endpoint_id);
    return ESP_OK;
}

static void wb_text_set_topic(void *device_data, const char *topic)
{
    wb_text_data_t *data = (wb_text_data_t *)device_data;
    if (data && topic)
        strncpy(data->topic, topic, sizeof(data->topic) - 1);
}

static esp_err_t wb_text_handle_mqtt(const char *payload, void *device_data)
{
    wb_text_data_t *data = (wb_text_data_t *)device_data;
    if (!data || !payload) return ESP_ERR_INVALID_ARG;
    strncpy(data->value, payload, sizeof(data->value) - 1);
    ESP_LOGD(TAG, "WB text ep=%d value=\"%s\"", data->endpoint_id, data->value);
    return ESP_OK;
}

static esp_err_t wb_text_handle_zigbee(void *zcl_cmd, void *device_data)
{
    return ESP_OK;
}

static esp_err_t wb_text_publish_state(void *device_data, const char *topic)
{
    return ESP_OK;
}

static size_t    wb_text_get_config_size(void) { return sizeof(wb_text_data_t); }

static esp_err_t wb_text_save_config(const void *d, char *blob, size_t *len)
{ *len = sizeof(wb_text_data_t); memcpy(blob, d, *len); return ESP_OK; }

static esp_err_t wb_text_load_config(void *d, const char *blob, size_t len)
{ memcpy(d, blob, len); return ESP_OK; }

static void wb_text_free_data(void *d) { if (d) free(d); }

static const device_driver_interface_t s_wb_text_driver = {
    .device_type   = "wb_text",
    .display_name  = "WB Text",
    .description   = "Wirenboard text control — MQTT only, no Zigbee endpoint",
    .readonly      = false,  /* can be writable; handled by topic/on subscription */
    .properties    = "[]",
    .init_endpoint = wb_text_init_endpoint,
    .set_topic     = wb_text_set_topic,
    .handle_mqtt   = wb_text_handle_mqtt,
    .handle_zigbee = wb_text_handle_zigbee,
    .publish_state = wb_text_publish_state,
    .get_config_size = wb_text_get_config_size,
    .save_config   = wb_text_save_config,
    .load_config   = wb_text_load_config,
    .free_data     = wb_text_free_data,
};

void driver_wb_text_register(void)
{
    device_driver_register(&s_wb_text_driver);
}
