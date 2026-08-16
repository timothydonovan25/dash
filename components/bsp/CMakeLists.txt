idf_component_register(
    SRCS "src/buses.cpp"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_spi esp_driver_i2c esp_driver_uart
             esp_driver_gpio esp_driver_ledc)
