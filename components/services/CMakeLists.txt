idf_component_register(
    SRCS "src/queues.cpp"
         "src/ctrl_service.cpp"
         "src/imu_service.cpp"
         "src/gnss_service.cpp"
         "src/logic_service.cpp"
         "src/logic_tick.cpp"
         "src/input_service.cpp"
         "src/render_service.cpp"
         "src/keeper_service.cpp"
         "src/logger_service.cpp"
         "src/storage_service.cpp"
         "src/ble_service.cpp"
         "src/c6_firmware.cpp"
         "src/ota_service.cpp"
         "src/settings_store.cpp"
    INCLUDE_DIRS "include"
    REQUIRES core dash_hal ui bsp drivers esp_timer esp_driver_gpio
    PRIV_REQUIRES bt esp_hosted esp_partition bootloader_support app_update
                  esp_app_format)

# NimBLE's service and characteristic tables end in a bare {} sentinel,
# which is normal C and not fully specified enough for this warning in C++.
set_source_files_properties(src/ble_service.cpp PROPERTIES
    COMPILE_OPTIONS "-Wno-missing-field-initializers")
