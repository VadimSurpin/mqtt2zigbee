idf_component_register(
    SRCS "zigbee_coordinator.c"
    INCLUDE_DIRS "."
    REQUIRES esp-zigbee-lib freertos
)
