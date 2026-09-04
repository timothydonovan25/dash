idf_component_register(
    SRCS "main.cpp"
         "p4_panel.cpp"
         "touch_buttons.cpp"
         "demo_render.cpp"
         "demo_bike_task.cpp"
         "ble_media.cpp"
         "ota_slave.cpp"
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
             esp_timer freertos
             esp_hosted bt esp_partition bootloader_support)

# Pushes demo_p4/c6_cp's built firmware into the slave_fw data partition
# (see partitions.csv) at `idf.py flash` time, so main/ota_slave.cpp has
# something to send the C6 over SDIO. `c6_cp` builds as its own separate
# IDF project (own target, esp32c6) — it isn't built as part of this one —
# so this just picks up whatever's already in its build/ directory; build
# it first with `idf.py -C ../c6_cp build`.
set(C6_CP_BIN "${CMAKE_SOURCE_DIR}/../c6_cp/build/network_adapter.bin")
if(EXISTS "${C6_CP_BIN}")
    esptool_py_flash_to_partition(flash "slave_fw" "${C6_CP_BIN}")
else()
    message(WARNING "demo_p4: ${C6_CP_BIN} not found — build c6_cp first "
                     "(idf.py -C ../c6_cp build) or `idf.py flash` will "
                     "leave the slave_fw partition empty.")
endif()

# ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG() itself expands to a partial designated
# initializer — not our code, and not worth failing the build over. Same
# story for NimBLE's ble_gatt_svc_def/ble_gatt_chr_def sentinels in
# ble_media.cpp: a bare {} terminator is normal C but not fully-specified
# enough for this warning in C++.
set_source_files_properties(touch_buttons.cpp ble_media.cpp PROPERTIES
    COMPILE_OPTIONS "-Wno-missing-field-initializers")
