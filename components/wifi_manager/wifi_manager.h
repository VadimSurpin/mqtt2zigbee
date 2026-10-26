#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"

void wifi_manager_init(void);
void wifi_manager_ap_init(const char *ssid, const char *password);
void wifi_manager_ap_stop(void);
void wifi_manager_sta_init(void);
esp_err_t wifi_manager_connect(const char *ssid, const char *password);
void wifi_manager_disconnect(void);
void wifi_manager_register_events(void (*event_handler)(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data));
bool wifi_manager_is_connected(void);
char* wifi_manager_get_ip(void);
void wifi_manager_toggle_ap(const char *ap_ssid);
bool wifi_manager_ap_active(void);
/* Enable/disable the setup AP. Disabling is refused (returns false) unless STA is connected. */
bool wifi_manager_set_ap(bool on, const char *ap_ssid);
/* Copies the SSID of the router we are associated with; false if not connected. */
bool wifi_manager_get_sta_ssid(char *out, size_t len);

#endif
