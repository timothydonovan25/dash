idf_component_register(
    SRCS "src/clock.cpp"
         "src/mcp23s17.cpp"
         "src/pod.cpp"
         "src/engine_sense.cpp"
         "src/pa1616d.cpp"
         "src/icm42688p.cpp"
         "src/ili9488.cpp"
         "src/nvs_storage.cpp"
         "src/sdcard.cpp"
    INCLUDE_DIRS "include"
    REQUIRES dash_hal bsp core
             esp_driver_spi esp_driver_i2c esp_driver_uart
             esp_driver_gpio esp_driver_ledc
             esp_lcd esp_timer nvs_flash
             esp_driver_sdspi fatfs vfs)
