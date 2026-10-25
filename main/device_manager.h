#ifndef DEVICE_MANAGER_H
#define DEVICE_MANAGER_H

#include <stdint.h>
#include "esp_err.h"

typedef enum {
    DEVICE_EVENT_STATE_CHANGED,
    DEVICE_EVENT_CONFIG_UPDATED
} device_event_t;

typedef void (*device_event_handler_t)(uint8_t endpoint_id, const char *topic, const char *payload);

esp_err_t device_manager_init(void);
esp_err_t device_manager_add_device(uint8_t endpoint_id, const char *device_type, const char *topic, const char *alias, const char *mfr_name, const char *model_id, const char *zb_subtype);
esp_err_t device_manager_remove_device(uint8_t endpoint_id);
esp_err_t device_manager_get_all_devices(char *json_buffer, size_t buffer_size);
esp_err_t device_manager_handle_mqtt_message(const char *topic, const char *payload);
esp_err_t device_manager_handle_zigbee_attribute(uint8_t endpoint_id, uint16_t cluster,
                                                   uint16_t attr_id, const void *value,
                                                   uint16_t size, uint8_t data_type);
esp_err_t device_manager_publish_state(uint8_t endpoint_id);
void device_manager_set_event_handler(device_event_handler_t handler);
esp_err_t device_manager_restore_from_nvs(void);
void      device_manager_build_zigbee_endpoints(void);
esp_err_t device_manager_save_to_nvs(void);

#endif
