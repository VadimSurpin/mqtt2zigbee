#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define APP_CONFIG_NAMESPACE "app_config"
#define WIFI_SSID_KEY "wifi_ssid"
#define WIFI_PASS_KEY "wifi_pass"
#define MQTT_HOST_KEY "mqtt_host"
#define MQTT_PORT_KEY "mqtt_port"
#define MQTT_USE_TLS_KEY "mqtt_tls"
#define MQTT_CLIENT_ID_KEY "mqtt_cli_id"
#define MQTT_USER_KEY "mqtt_user"
#define MQTT_PASS_KEY "mqtt_pass"
#define PROVISIONED_KEY "provisioned"
#define UI_USER_KEY     "ui_user"
#define UI_PASS_KEY     "ui_pass"
#define UI_LANG_KEY     "ui_lang"
#define DISC_MODE_KEY   "disc_mode"

/* Discovery source shown on the Devices page. */
#define DISC_MODE_WB      0   /* Wiren Board, built-in virtual devices hidden */
#define DISC_MODE_WB_ALL  1   /* Wiren Board incl. built-in devices */
#define DISC_MODE_Z2M     2   /* Zigbee2MQTT */
#define DISC_MODE_AUTO    255 /* not chosen yet: WB if WB topics exist, else Z2M */

typedef struct {
    char wifi_ssid[32];
    char wifi_password[64];
    char mqtt_host[64];
    uint16_t mqtt_port;
    bool mqtt_use_tls;
    char mqtt_client_id[32];
    char mqtt_username[32];
    char mqtt_password[64];
    bool provisioned;
    char ui_user[32];
    char ui_pass[64];
    char ui_lang[4];
    uint8_t disc_mode;
} app_config_data_t;

void app_config_init(void);
esp_err_t app_config_save_wifi(const char *ssid, const char *password);
esp_err_t app_config_save_mqtt(const char *host, uint16_t port, bool use_tls,
                                const char *client_id, const char *username, const char *password);
esp_err_t app_config_save_devices(const char *devices_json, size_t len);
esp_err_t app_config_load_devices(char *devices_json, size_t *len);
const char* app_config_get_devices(void);
bool app_config_is_provisioned(void);
const char* app_config_get_wifi_ssid(void);
const char* app_config_get_wifi_password(void);
const char* app_config_get_mqtt_host(void);
uint16_t app_config_get_mqtt_port(void);
bool app_config_get_mqtt_use_tls(void);
const char* app_config_get_mqtt_client_id(void);
const char* app_config_get_mqtt_username(void);
const char* app_config_get_mqtt_password(void);
esp_err_t app_config_factory_reset(void);
esp_err_t app_config_set_provisioned(bool value);
esp_err_t app_config_save_ui_creds(const char *username, const char *password);
const char* app_config_get_ui_user(void);
const char* app_config_get_ui_pass(void);
esp_err_t app_config_set_language(const char *lang);
const char* app_config_get_language(void);
esp_err_t app_config_set_discovery_mode(uint8_t mode);
uint8_t app_config_get_discovery_mode(void);

#endif
