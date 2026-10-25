#ifndef DEVICE_DRIVER_H
#define DEVICE_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct device_driver device_driver_t;

typedef struct {
    const char *device_type;
    const char *display_name;
    const char *description;
    bool readonly;
    const char *properties;
    esp_err_t (*init_endpoint)(uint8_t endpoint_id, const void *config);
    void      (*set_topic)(void *device_data, const char *topic); /* optional; called after alloc */
    esp_err_t (*handle_mqtt)(const char *payload, void *device_data);
    esp_err_t (*handle_zigbee)(void *zcl_cmd, void *device_data);
    esp_err_t (*publish_state)(void *device_data, const char *topic);
    size_t (*get_config_size)(void);
    esp_err_t (*save_config)(const void *device_data, char *blob, size_t *len);
    esp_err_t (*load_config)(void *device_data, const char *blob, size_t len);
    void (*free_data)(void *device_data);
} device_driver_interface_t;

struct device_driver {
    const device_driver_interface_t *iface;
    const char *device_type;
    bool registered;
};

esp_err_t device_driver_register(const device_driver_interface_t *driver);
const device_driver_t* device_driver_find(const char *device_type);
void device_driver_get_all_types(char *json_buffer, size_t buffer_size);
void device_drivers_init(void);

#endif
