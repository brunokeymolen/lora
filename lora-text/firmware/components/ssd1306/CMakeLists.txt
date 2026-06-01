idf_component_register(
    SRCS "ssd1306.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver esp_driver_i2c
)
