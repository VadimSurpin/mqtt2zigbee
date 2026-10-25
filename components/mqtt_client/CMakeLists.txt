idf_component_register(
    SRCS "mqtt_client_wrapper.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES main
    REQUIRES mqtt
)
