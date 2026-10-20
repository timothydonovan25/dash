idf_component_register(
    SRCS "src/queues.cpp"
         "src/ctrl_service.cpp"
         "src/imu_service.cpp"
         "src/gnss_service.cpp"
         "src/logic_service.cpp"
         "src/logic_tick.cpp"
         "src/track_store.cpp"
         "src/input_service.cpp"
         "src/render_service.cpp"
         "src/keeper_service.cpp"
         "src/power_service.cpp"
         "src/logger_service.cpp"
         "src/storage_service.cpp"
         "src/ble_service.cpp"
         "src/c6_firmware.cpp"
         "src/ota_service.cpp"
         "src/settings_store.cpp"
         "src/wifi_service.cpp"
         "src/wifi_page.cpp"
         "src/stack_watch.cpp"
         "src/watchdog.cpp"
    INCLUDE_DIRS "include"
    REQUIRES core dash_hal ui bsp drivers esp_timer esp_driver_gpio
    PRIV_REQUIRES esp_system bt esp_hosted esp_partition bootloader_support app_update
                  esp_app_format esp_wifi esp_wifi_remote esp_netif esp_event
                  esp_http_server)

# NimBLE's service and characteristic tables end in a bare {} sentinel,
# which is normal C and not fully specified enough for this warning in C++.
set_source_files_properties(src/ble_service.cpp PROPERTIES
    COMPILE_OPTIONS "-Wno-missing-field-initializers")
