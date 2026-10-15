idf_component_register(
    SRCS "main.cpp"
         "p4_panel.cpp"
         "touch_buttons.cpp"
         "pod_buttons.cpp"
         "demo_render.cpp"
         "demo_bike_task.cpp"
         "ctrl_probe.cpp"
         "serial_buttons.cpp"
         # Hardware-free real firmware code, reused by source rather than by
         # depending on the components/services or components/drivers IDF
         # components — those REQUIRE bsp/drivers, which are wired for the
         # real board's S3 pin map and would have no business in a P4 build.
         # See docs/bringup-p4-demo.md.
         "../../components/services/src/queues.cpp"
         "../../components/services/src/logic_tick.cpp"
         # The production services this board can run as they are: the
         # stable/dev slots, BLE with bonding and the C6 updater, and the
         # settings half of keeper. The point is to test these, not copies.
         "../../components/services/src/ota_service.cpp"
         "../../components/services/src/ble_service.cpp"
         "../../components/services/src/c6_firmware.cpp"
         "../../components/services/src/settings_store.cpp"
         "../../components/services/src/wifi_service.cpp"
         "../../components/services/src/wifi_page.cpp"
         "../../components/drivers/src/nvs_storage.cpp"
         "../../components/drivers/src/clock.cpp"
         "../../components/drivers/src/chip_temp.cpp"
    INCLUDE_DIRS "."
                 "../../components/services/include"
                 "../../components/services/src"
                 "../../components/drivers/include"
    REQUIRES core dash_hal ui p4_restart_fix
             esp_lcd esp_mm esp_driver_tsens esp_driver_i2c esp_driver_gpio esp_driver_ledc
             esp_timer freertos esp_driver_usb_serial_jtag
             esp_hosted bt esp_partition bootloader_support
             app_update esp_app_format nvs_flash
             esp_wifi esp_wifi_remote esp_netif esp_event esp_http_server)

# Pushes demo_p4/c6_cp's built firmware into the slave_fw data partition
# (see ../partitions.csv) with `idf.py c6-flash`, so the production
# services/src/c6_firmware.cpp has something to send the C6 over SDIO. `c6_cp` builds as its own separate
# IDF project (own target, esp32c6) — it isn't built as part of this one —
# so this just picks up whatever's already in its build/ directory; build
# it first with `idf.py -C ../c6_cp build`.
set(C6_CP_BIN "${CMAKE_SOURCE_DIR}/c6_cp/build/network_adapter.bin")
if(EXISTS "${C6_CP_BIN}")
    # Its own target, not part of `idf.py flash`: esptool refuses to write
    # an image built for another chip unless forced, and forcing the whole
    # flash would also skip those checks for the P4's own images. A plain
    # flash never touches slave_fw, so this is once per board, and again
    # only when c6_cp changes:  idf.py -p /dev/ttyACM0 c6-flash
    idf_component_get_property(c6_main_args esptool_py FLASH_ARGS)
    idf_component_get_property(c6_sub_args esptool_py FLASH_SUB_ARGS)
    esptool_py_flash_target(c6-flash "${c6_main_args}" "${c6_sub_args} --force")
    esptool_py_flash_to_partition(c6-flash "slave_fw" "${C6_CP_BIN}")
else()
    message(WARNING "demo_p4: ${C6_CP_BIN} not found, so there is no `idf.py "
                    "c6-flash` and the C6 keeps whatever it has. Build "
                    "idf.py -C c6_cp build first.")
endif()

# ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG() itself expands to a partial designated
# initializer — not our code, and not worth failing the build over. Same
# story for NimBLE's ble_gatt_svc_def/ble_gatt_chr_def sentinels in
# ble_media.cpp: a bare {} terminator is normal C but not fully-specified
# enough for this warning in C++.
set_source_files_properties(touch_buttons.cpp
    ../../components/services/src/ble_service.cpp PROPERTIES
    COMPILE_OPTIONS "-Wno-missing-field-initializers")

# A dev image that aborts at boot, for the rollback test: idf.py -B
# build-crash -DDASH_CRASH_TEST=1 build. Off in any normal build.
if(DASH_CRASH_TEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DASH_CRASH_TEST=1)
endif()
