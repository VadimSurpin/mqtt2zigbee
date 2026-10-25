#ifndef ZIGBEE_COORDINATOR_H
#define ZIGBEE_COORDINATOR_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* Callback type for attribute-report / set-attr events.
   Registered by main.c to forward data to device_manager without
   creating a circular component dependency. */
typedef void (*zigbee_attr_report_cb_t)(uint8_t endpoint_id, uint16_t cluster,
                                         uint16_t attr_id, const void *value,
                                         uint16_t size, uint8_t data_type);

/* Called when the device successfully joins a Zigbee network. */
typedef void (*zigbee_network_join_cb_t)(uint8_t channel, uint16_t pan_id, uint16_t short_addr);

void zigbee_coordinator_set_attr_report_callback(zigbee_attr_report_cb_t cb);
void zigbee_coordinator_set_network_join_callback(zigbee_network_join_cb_t cb);

/* Set manufacturer name and model identifier that will be embedded in the
   Basic cluster of the NEXT endpoint created by zigbee_coordinator_add_*_endpoint.
   Call once before each add_*_endpoint call; cleared automatically afterwards. */
void zigbee_coordinator_set_basic_info(const char *mfr_name, const char *model_id);

/* endpoint_setup_cb is called from zigbee_main_task after esp_zb_init() —
   safe to call esp_zb_* endpoint/cluster registration functions there. */
void zigbee_coordinator_start(void (*endpoint_setup_cb)(void));
void zigbee_coordinator_stop(void);
/* Send a ZDO Leave to the coordinator then erase Zigbee NVS and reboot.
   Safe to call from any task — executes inside the Zigbee scheduler. */
void zigbee_coordinator_leave_network(void);

esp_err_t zigbee_coordinator_add_onoff_endpoint(uint8_t endpoint_id);
esp_err_t zigbee_coordinator_add_switch_endpoint(uint8_t endpoint_id);
esp_err_t zigbee_coordinator_add_temperature_endpoint(uint8_t endpoint_id);
esp_err_t zigbee_coordinator_add_humidity_endpoint(uint8_t endpoint_id);
esp_err_t zigbee_coordinator_add_pressure_endpoint(uint8_t endpoint_id);
esp_err_t zigbee_coordinator_add_contact_endpoint(uint8_t endpoint_id);
esp_err_t zigbee_coordinator_add_rgb_light_endpoint(uint8_t endpoint_id);
esp_err_t zigbee_coordinator_add_analog_input_endpoint(uint8_t endpoint_id);
esp_err_t zigbee_coordinator_add_level_endpoint(uint8_t endpoint_id);

esp_err_t zigbee_coordinator_set_onoff(uint8_t endpoint_id, bool on);
esp_err_t zigbee_coordinator_set_rgb_light(uint8_t endpoint_id, bool on, uint8_t brightness,
                                             uint8_t r, uint8_t g, uint8_t b);
esp_err_t zigbee_coordinator_set_temperature(uint8_t endpoint_id, float celsius);
esp_err_t zigbee_coordinator_set_humidity(uint8_t endpoint_id, float percent);
esp_err_t zigbee_coordinator_set_pressure(uint8_t endpoint_id, float hpa);
esp_err_t zigbee_coordinator_set_contact(uint8_t endpoint_id, bool open);
esp_err_t zigbee_coordinator_set_analog_input(uint8_t endpoint_id, float value);
esp_err_t zigbee_coordinator_set_level(uint8_t endpoint_id, uint8_t level);

#endif
