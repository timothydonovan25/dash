# DIAG -> Doom on either board (include/doom/doom.hpp): doomgeneric's
# platform layer and the menu row. Each board's main supplies a doom::Board.
idf_component_register(SRCS "src/doom.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES core dash_hal ui doomgeneric freertos esp_timer heap log)
