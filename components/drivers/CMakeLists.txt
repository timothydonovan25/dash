idf_component_register(
    SRCS "src/clock.cpp"
         "src/pod.cpp"
         "src/engine_sense.cpp"
         "src/pa1616d.cpp"
         "src/icm42688p.cpp"
         "src/ili9488.cpp"
         "src/pca9955b.cpp"
         "src/vin_sense.cpp"
         "src/nvs_storage.cpp"
         "src/sdcard.cpp"
         "src/chip_temp.cpp"
    INCLUDE_DIRS "include"
    REQUIRES dash_hal bsp core
             esp_driver_spi esp_driver_i2c esp_driver_uart
             esp_driver_gpio esp_adc
             esp_lcd esp_timer nvs_flash
             esp_driver_sdspi fatfs vfs esp_driver_tsens)
