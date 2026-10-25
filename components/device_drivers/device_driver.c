#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "device_driver.h"

static const char *TAG = "DEVICE_DRIVER_REG";

#define MAX_DRIVERS 32

static device_driver_t s_drivers[MAX_DRIVERS];
static int s_driver_count = 0;

esp_err_t device_driver_register(const device_driver_interface_t *driver_iface)
{
    if (!driver_iface || !driver_iface->device_type) {
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 0; i < s_driver_count; i++) {
        if (strcmp(s_drivers[i].device_type, driver_iface->device_type) == 0) {
            ESP_LOGW(TAG, "Driver %s already registered", driver_iface->device_type);
            return ESP_OK;
        }
    }

    if (s_driver_count >= MAX_DRIVERS) {
        ESP_LOGE(TAG, "Maximum number of drivers reached");
        return ESP_ERR_NO_MEM;
    }

    s_drivers[s_driver_count].iface = driver_iface;
    s_drivers[s_driver_count].device_type = driver_iface->device_type;
    s_drivers[s_driver_count].registered = true;
    s_driver_count++;

    ESP_LOGI(TAG, "Registered driver: %s", driver_iface->device_type);
    return ESP_OK;
}

const device_driver_t* device_driver_find(const char *device_type)
{
    if (!device_type) return NULL;

    for (int i = 0; i < s_driver_count; i++) {
        if (s_drivers[i].registered && strcmp(s_drivers[i].device_type, device_type) == 0) {
            return &s_drivers[i];
        }
    }
    return NULL;
}

void device_driver_get_all_types(char *json_buffer, size_t buffer_size)
{
    int offset = 0;
    offset += snprintf(json_buffer + offset, buffer_size - offset, "[");

    for (int i = 0; i < s_driver_count; i++) {
        if (!s_drivers[i].registered) continue;
        
        if (offset > 1) offset += snprintf(json_buffer + offset, buffer_size - offset, ",");
        
        const char *display = s_drivers[i].iface->display_name ? s_drivers[i].iface->display_name : s_drivers[i].device_type;
        const char *desc = s_drivers[i].iface->description ? s_drivers[i].iface->description : "";
        const char *props = s_drivers[i].iface->properties ? s_drivers[i].iface->properties : "[]";
        
        offset += snprintf(json_buffer + offset, buffer_size - offset,
            "{\"type\":\"%s\",\"display_name\":\"%s\",\"description\":\"%s\",\"readonly\":%s,\"properties\":%s}",
            s_drivers[i].device_type, display, desc,
            s_drivers[i].iface->readonly ? "true" : "false",
            props);
    }
    offset += snprintf(json_buffer + offset, buffer_size - offset, "]");
}

void device_drivers_init(void)
{
    ESP_LOGI(TAG, "Initializing device drivers...");
    
    extern void driver_onoff_switch_register(void);
    extern void driver_temperature_sensor_register(void);
    extern void driver_humidity_sensor_register(void);
    extern void driver_pressure_sensor_register(void);
    extern void driver_contact_sensor_register(void);
    extern void driver_rgb_light_register(void);
    extern void driver_wb_switch_register(void);
    extern void driver_wb_sensor_register(void);
    extern void driver_wb_range_register(void);
    extern void driver_wb_text_register(void);
    extern void driver_z2m_register(void);

    driver_onoff_switch_register();
    driver_temperature_sensor_register();
    driver_humidity_sensor_register();
    driver_pressure_sensor_register();
    driver_contact_sensor_register();
    driver_rgb_light_register();
    driver_wb_switch_register();
    driver_wb_sensor_register();
    driver_wb_range_register();
    driver_wb_text_register();
    driver_z2m_register();

    ESP_LOGI(TAG, "Registered %d device drivers", s_driver_count);
}
