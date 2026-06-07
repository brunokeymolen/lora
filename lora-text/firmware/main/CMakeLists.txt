idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        ra01s ssd1306
        driver esp_driver_gpio esp_system freertos
        bt nvs_flash mbedtls esp_hw_support
    PRIV_REQUIRES esp_timer
)
