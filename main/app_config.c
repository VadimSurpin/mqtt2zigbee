#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "app_config.h"

static const char *TAG = "APP_CONFIG";
static app_config_data_t s_config = {0};
static bool s_initialized = false;

void app_config_init(void)
{
    if (s_initialized) return;

    /* Apply defaults first — these are overwritten below if NVS has values. */
    strncpy(s_config.ui_user, "admin",   sizeof(s_config.ui_user)  - 1);
    strncpy(s_config.ui_pass, "gateway", sizeof(s_config.ui_pass) - 1);
    s_config.disc_mode = DISC_MODE_AUTO;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(APP_CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        /* Fresh NVS (e.g. after factory reset) — defaults are already set. */
        ESP_LOGW(TAG, "NVS not opened (%s), using defaults", esp_err_to_name(err));
        s_initialized = true;
        return;
    }

    size_t len = sizeof(s_config.wifi_ssid);
    nvs_get_str(handle, WIFI_SSID_KEY, s_config.wifi_ssid, &len);

    len = sizeof(s_config.wifi_password);
    nvs_get_str(handle, WIFI_PASS_KEY, s_config.wifi_password, &len);

    len = sizeof(s_config.mqtt_host);
    nvs_get_str(handle, MQTT_HOST_KEY, s_config.mqtt_host, &len);

    nvs_get_u16(handle, MQTT_PORT_KEY, &s_config.mqtt_port);
    nvs_get_u8(handle, MQTT_USE_TLS_KEY, (uint8_t*)&s_config.mqtt_use_tls);

    len = sizeof(s_config.mqtt_client_id);
    nvs_get_str(handle, MQTT_CLIENT_ID_KEY, s_config.mqtt_client_id, &len);

    len = sizeof(s_config.mqtt_username);
    nvs_get_str(handle, MQTT_USER_KEY, s_config.mqtt_username, &len);

    len = sizeof(s_config.mqtt_password);
    nvs_get_str(handle, MQTT_PASS_KEY, s_config.mqtt_password, &len);

    nvs_get_u8(handle, PROVISIONED_KEY, (uint8_t*)&s_config.provisioned);

    /* Override credential defaults only if stored values exist. */
    char tmp[64] = {0};
    len = sizeof(tmp);
    if (nvs_get_str(handle, UI_USER_KEY, tmp, &len) == ESP_OK && tmp[0] != '\0')
        strncpy(s_config.ui_user, tmp, sizeof(s_config.ui_user) - 1);
    len = sizeof(tmp);
    if (nvs_get_str(handle, UI_PASS_KEY, tmp, &len) == ESP_OK && tmp[0] != '\0')
        strncpy(s_config.ui_pass, tmp, sizeof(s_config.ui_pass) - 1);
    len = sizeof(s_config.ui_lang);
    nvs_get_str(handle, UI_LANG_KEY, s_config.ui_lang, &len);
    uint8_t mode = DISC_MODE_AUTO;
    if (nvs_get_u8(handle, DISC_MODE_KEY, &mode) == ESP_OK && mode <= DISC_MODE_Z2M)
        s_config.disc_mode = mode;

    nvs_close(handle);

    s_initialized = true;
    ESP_LOGI(TAG, "Config loaded, provisioned: %d, user: %s", s_config.provisioned, s_config.ui_user);
}

esp_err_t app_config_save_wifi(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(APP_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    strncpy(s_config.wifi_ssid, ssid, sizeof(s_config.wifi_ssid) - 1);
    strncpy(s_config.wifi_password, password, sizeof(s_config.wifi_password) - 1);

    err = nvs_set_str(handle, WIFI_SSID_KEY, s_config.wifi_ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, WIFI_PASS_KEY, s_config.wifi_password);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t app_config_save_mqtt(const char *host, uint16_t port, bool use_tls,
                                const char *client_id, const char *username, const char *password)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(APP_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    strncpy(s_config.mqtt_host, host, sizeof(s_config.mqtt_host) - 1);
    s_config.mqtt_port = port;
    s_config.mqtt_use_tls = use_tls;
    strncpy(s_config.mqtt_client_id, client_id, sizeof(s_config.mqtt_client_id) - 1);
    strncpy(s_config.mqtt_username, username, sizeof(s_config.mqtt_username) - 1);
    strncpy(s_config.mqtt_password, password, sizeof(s_config.mqtt_password) - 1);

    err = nvs_set_str(handle, MQTT_HOST_KEY, s_config.mqtt_host);
    if (err == ESP_OK) err = nvs_set_u16(handle, MQTT_PORT_KEY, s_config.mqtt_port);
    if (err == ESP_OK) err = nvs_set_u8(handle, MQTT_USE_TLS_KEY, s_config.mqtt_use_tls);
    if (err == ESP_OK) err = nvs_set_str(handle, MQTT_CLIENT_ID_KEY, s_config.mqtt_client_id);
    if (err == ESP_OK) err = nvs_set_str(handle, MQTT_USER_KEY, s_config.mqtt_username);
    if (err == ESP_OK) err = nvs_set_str(handle, MQTT_PASS_KEY, s_config.mqtt_password);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t app_config_save_devices(const char *devices_json, size_t len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(APP_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    
    err = nvs_set_blob(handle, "devices_json", devices_json, len);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t app_config_load_devices(char *devices_json, size_t *len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(APP_CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    
    err = nvs_get_blob(handle, "devices_json", devices_json, len);
    nvs_close(handle);
    return err;
}

static char s_devices_json_buf[4096];

const char* app_config_get_devices(void)
{
    size_t len = sizeof(s_devices_json_buf);
    esp_err_t err = app_config_load_devices(s_devices_json_buf, &len);
    if (err != ESP_OK || len == 0) {
        return NULL;
    }
    s_devices_json_buf[len] = '\0';
    return s_devices_json_buf;
}

bool app_config_is_provisioned(void)
{
    if (!s_initialized) app_config_init();
    return s_config.provisioned;
}

const char* app_config_get_wifi_ssid(void)
{
    return s_config.wifi_ssid;
}

const char* app_config_get_wifi_password(void)
{
    return s_config.wifi_password;
}

const char* app_config_get_mqtt_host(void)
{
    return s_config.mqtt_host;
}

uint16_t app_config_get_mqtt_port(void)
{
    return s_config.mqtt_port;
}

bool app_config_get_mqtt_use_tls(void)
{
    return s_config.mqtt_use_tls;
}

const char* app_config_get_mqtt_client_id(void)
{
    return s_config.mqtt_client_id;
}

const char* app_config_get_mqtt_username(void)
{
    return s_config.mqtt_username;
}

const char* app_config_get_mqtt_password(void)
{
    return s_config.mqtt_password;
}

esp_err_t app_config_factory_reset(void)
{
    ESP_LOGI(TAG, "Factory reset triggered — erasing NVS");

    /* Erase the entire NVS flash partition so no namespace survives. */
    esp_err_t err = nvs_flash_erase();
    ESP_LOGI(TAG, "nvs_flash_erase: %s", esp_err_to_name(err));

    /* Re-initialise NVS so the rest of the system stays functional during
       the 1-second delay before reboot. */
    nvs_flash_init();

    memset(&s_config, 0, sizeof(s_config));
    s_initialized = false;

    /* Clear the WiFi driver's stored credentials. */
    esp_wifi_restore();

    ESP_LOGI(TAG, "Factory reset complete, rebooting…");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

esp_err_t app_config_set_provisioned(bool value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(APP_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    s_config.provisioned = value;
    err = nvs_set_u8(handle, PROVISIONED_KEY, value);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t app_config_save_ui_creds(const char *username, const char *password)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(APP_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    strncpy(s_config.ui_user, username, sizeof(s_config.ui_user) - 1);
    strncpy(s_config.ui_pass, password, sizeof(s_config.ui_pass) - 1);

    err = nvs_set_str(handle, UI_USER_KEY, s_config.ui_user);
    if (err == ESP_OK) err = nvs_set_str(handle, UI_PASS_KEY, s_config.ui_pass);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

const char* app_config_get_ui_user(void)
{
    if (!s_initialized) app_config_init();
    return s_config.ui_user;
}

const char* app_config_get_ui_pass(void)
{
    if (!s_initialized) app_config_init();
    return s_config.ui_pass;
}

esp_err_t app_config_set_language(const char *lang)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(APP_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    strncpy(s_config.ui_lang, lang, sizeof(s_config.ui_lang) - 1);
    err = nvs_set_str(handle, UI_LANG_KEY, s_config.ui_lang);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

const char* app_config_get_language(void)
{
    if (!s_initialized) app_config_init();
    return s_config.ui_lang[0] ? s_config.ui_lang : "en";
}

esp_err_t app_config_set_discovery_mode(uint8_t mode)
{
    if (mode > DISC_MODE_Z2M) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(APP_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    s_config.disc_mode = mode;
    err = nvs_set_u8(handle, DISC_MODE_KEY, mode);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

uint8_t app_config_get_discovery_mode(void)
{
    if (!s_initialized) app_config_init();
    return s_config.disc_mode;
}
