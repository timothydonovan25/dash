idf_component_register(
    SRCS "main.cpp"
         "p4_panel.cpp"
         "touch_buttons.cpp"
         "demo_render.cpp"
         "demo_bike_task.cpp"
         # Hardware-free real firmware code, reused by source rather than by
         # depending on the components/services or components/drivers IDF
         # components — those REQUIRE bsp/drivers, which are wired for the
         # real board's S3 pin map and would have no business in a P4 build.
         # See docs/bringup-p4-demo.md.
         "../../components/services/src/queues.cpp"
         "../../components/services/src/logic_tick.cpp"
         "../../components/drivers/src/clock.cpp"
         "../../sim/sim_bike.cpp"
    INCLUDE_DIRS "."
                 "../../components/services/include"
                 "../../components/drivers/include"
                 "../../sim"
    REQUIRES core dash_hal ui
             esp_lcd esp_driver_i2c esp_driver_gpio esp_driver_ledc
             esp_timer freertos)

# ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG() itself expands to a partial designated
# initializer — not our code, and not worth failing the build over.
set_source_files_properties(touch_buttons.cpp PROPERTIES
    COMPILE_OPTIONS "-Wno-missing-field-initializers")
