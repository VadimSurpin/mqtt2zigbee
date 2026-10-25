idf_component_register(
    SRCS "main.c" "app_config.c" "device_manager.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES button_handler
    REQUIRES spi_flash esp_wifi esp_netif esp_http_server mqtt nvs_flash spiffs device_drivers wifi_manager mqtt_client web_server web_ui esp-zigbee-lib cjson status_led wb_driver z2m_driver
)
