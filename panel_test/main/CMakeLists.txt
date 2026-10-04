idf_component_register(
    SRCS "panel_test.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd esp_driver_spi esp_driver_gpio esp_timer freertos)
