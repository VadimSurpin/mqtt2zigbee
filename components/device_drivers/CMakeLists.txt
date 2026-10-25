idf_component_register(
    SRCS "device_driver.c"
        "driver_onoff_switch.c"
        "driver_temperature_sensor.c"
        "driver_humidity_sensor.c"
        "driver_pressure_sensor.c"
        "driver_contact_sensor.c"
        "driver_rgb_light.c"
        "driver_wb_switch.c"
        "driver_wb_sensor.c"
        "driver_wb_range.c"
        "driver_wb_text.c"
        "driver_z2m.c"
    INCLUDE_DIRS "."
    REQUIRES esp-zigbee-lib nvs_flash mqtt_client zigbee_core
)
