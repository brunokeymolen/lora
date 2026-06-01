idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES ra01s ssd1306 driver esp_driver_gpio esp_system freertos
    PRIV_REQUIRES esp_timer
)
