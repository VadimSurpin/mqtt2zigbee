#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "wifi_manager.h"

static const char *TAG = "WIFI_MANAGER";
static bool s_connected     = false;
static bool s_auto_connect  = false;   /* only reconnect when we have a valid STA config */
static char s_ip[16] = {0};
static void (*s_event_handler)(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) = NULL;

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_auto_connect) esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        ESP_LOGI(TAG, "WiFi disconnected");
        if (s_auto_connect) esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
        ESP_LOGI(TAG, "Got IP: %s", s_ip);
    }
    if (s_event_handler) {
        s_event_handler(arg, event_base, event_id, event_data);
    }
}

void wifi_manager_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(sta_netif, "zigbee-mqtt-bridge");
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));
}

void wifi_manager_ap_init(const char *ssid, const char *password)
{
    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = password ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    if (password) {
        strncpy((char*)wifi_config.ap.password, password, sizeof(wifi_config.ap.password) - 1);
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* MIN_MODEM power save lets the coexistence arbiter share the radio
       between WiFi and Zigbee (IEEE 802.15.4) without dropping the link. */
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    ESP_LOGI(TAG, "AP started: %s", ssid);
}

void wifi_manager_ap_stop(void)
{
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_LOGI(TAG, "AP stopped");
}

void wifi_manager_sta_init(void)
{
    /* Stop AP if running (e.g. after provisioning in APSTA mode). Harmless if not started. */
    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "STA mode initialized");
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password)
{
    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password && password[0])
        strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = (password && password[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    s_auto_connect = true;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_LOGI(TAG, "Connecting to WiFi: %s", ssid);
    return esp_wifi_connect();
}

void wifi_manager_disconnect(void)
{
    s_auto_connect = false;
    esp_wifi_disconnect();
    s_connected = false;
}

void wifi_manager_register_events(void (*event_handler)(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data))
{
    s_event_handler = event_handler;
}

bool wifi_manager_is_connected(void)
{
    return s_connected;
}

char* wifi_manager_get_ip(void)
{
    return s_ip;
}

void wifi_manager_toggle_ap(const char *ap_ssid)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);

    if (mode == WIFI_MODE_APSTA) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        ESP_LOGI(TAG, "AP stopped — STA-only mode");
    } else {
        wifi_config_t ap_cfg = {0};
        strncpy((char *)ap_cfg.ap.ssid, ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
        ap_cfg.ap.max_connection = 4;
        ap_cfg.ap.authmode       = WIFI_AUTH_OPEN;
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
        ESP_LOGI(TAG, "AP started — AP+STA mode, SSID: %s", ap_ssid);
    }
}
