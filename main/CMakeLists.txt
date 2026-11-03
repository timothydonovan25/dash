idf_component_register(
    SRCS "main.cpp" "wiring.cpp" "serial_buttons.cpp" "doom_board.cpp"
    REQUIRES bsp core dash_hal ui drivers services doom p4_restart_fix
             esp_driver_usb_serial_jtag freertos)

# The C6's ESP-Hosted firmware, written into the `slave_fw` partition by
# `idf.py c6-flash` so services/src/c6_firmware.cpp has something to push
# to the C6 over SDIO. demo_p4/c6_cp is its own IDF project with its own
# target, so it is not built from here: build it first with
# `idf.py -C demo_p4/c6_cp set-target esp32c6 build`.
set(C6_CP_BIN "${CMAKE_SOURCE_DIR}/demo_p4/c6_cp/build/network_adapter.bin")
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
    message(WARNING "dash: ${C6_CP_BIN} not found, so there is no `idf.py "
                    "c6-flash` and the C6 keeps whatever it has. Build "
                    "idf.py -C demo_p4/c6_cp set-target esp32c6 build first.")
endif()
