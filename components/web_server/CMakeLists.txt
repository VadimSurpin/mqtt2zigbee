idf_component_register(
    SRCS "web_server.c" "locale.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES main 
    REQUIRES esp_http_server spiffs wifi_manager esp_wifi cjson web_ui wb_driver z2m_driver app_update esp_http_client
)
