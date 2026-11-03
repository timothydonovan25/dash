# Doom, as doomgeneric (github.com/ozkl/doomgeneric, e257a09), the engine
# only: its platform files (doomgeneric_*.c, the SDL and Allegro ones) are
# left out, and the dash's own is components/doom. GPL-2.0 (LICENSE),
# unlike the rest of this repo; both boards link it.
#
# Not our code, so none of our rules apply inside it: it is all globals,
# it mallocs, and its frames are its own (dash_frame_limit() is applied to
# components by name and this is not one of them). What it costs the rest
# of the firmware is kept in check from out here:
#   - its big zeroed arrays (~205 kB of its 244 kB of .bss) go to PSRAM
#     (DG_EXTRAM, src/dg_dash.h), and its 27 kB state table is read from
#     flash (linker.lf), not internal RAM, which the SDIO link, lwIP and
#     NimBLE share;
#   - its zone (6 MB) and its WAD directory are mallocs well over
#     CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL, so PSRAM too;
#   - it runs only after DIAG -> Doom, and on the bike only parked.
# Changes to its source: m_config.c's GetDefaultConfigDir() takes
# DOOMGENERIC_CONFIG_DIR, for default.cfg and saves on the card; eleven
# array definitions in r_*.c, d_loop.c and statdump.c carry DG_EXTRAM;
# I_Quit pauses in DG_Quit instead of exiting, and dg_dash.h turns every
# exit() into DG_Exit (doomgeneric.h), so leaving never resets the board.
file(GLOB srcs "${CMAKE_CURRENT_LIST_DIR}/src/*.c")
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "src"
                       LDFRAGMENTS "linker.lf"
                       REQUIRES vfs)

# 8-bit frames at Doom's own 320x200: the port turns them into RGB565 itself
# (one palette lookup per pixel) rather than doomgeneric expanding them to
# 32-bit first. Public, so doomgeneric.h means the same thing to the port.
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    CMAP256 DOOMGENERIC_RESX=320 DOOMGENERIC_RESY=200)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    DOOMGENERIC_CONFIG_DIR="/sd/games/")
# -O2 whatever the app's level: the firmware builds at -Og, and Doom's
# renderer is the one thing on this board that is CPU-bound. -w: 1990s C.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -w
    -include ${CMAKE_CURRENT_LIST_DIR}/src/dg_dash.h)
