idf_component_register(
    SRCS "wb_driver.c"
    INCLUDE_DIRS "."
    REQUIRES mqtt_client cjson
)
