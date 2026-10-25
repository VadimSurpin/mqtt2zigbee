idf_component_register(
    SRCS "status_led.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_rmt freertos
)
