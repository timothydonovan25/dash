idf_component_register(
    SRCS "main.cpp" "wiring.cpp"
    REQUIRES bsp core dash_hal ui drivers services)

# The C6's ESP-Hosted firmware, written into the `slave_fw` partition at
# `idf.py flash` time so services/src/c6_firmware.cpp has something to push
# to the C6 over SDIO. demo_p4/c6_cp is its own IDF project with its own
# target, so it is not built from here: build it first with
# `idf.py -C demo_p4/c6_cp set-target esp32c6 build`.
set(C6_CP_BIN "${CMAKE_SOURCE_DIR}/demo_p4/c6_cp/build/network_adapter.bin")
if(EXISTS "${C6_CP_BIN}")
    esptool_py_flash_to_partition(flash "slave_fw" "${C6_CP_BIN}")
else()
    message(WARNING "dash: ${C6_CP_BIN} not found. `idf.py flash` will leave "
                    "slave_fw empty and the C6 keeps whatever it shipped with.")
endif()
