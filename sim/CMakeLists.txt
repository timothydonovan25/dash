# The desktop simulator. Plain CMake, no ESP-IDF, like test/.
#
# It links the firmware's own core, ui and the transport-free half of
# services, so what it runs is what the bike runs. Only sim_bike.cpp is
# make-believe.
cmake_minimum_required(VERSION 3.16)
project(dash_sim CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Wpedantic)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(CORE ${ROOT}/components/core)
set(UI   ${ROOT}/components/ui)
set(HAL  ${ROOT}/components/dash_hal)
set(SVC  ${ROOT}/components/services)

add_library(core STATIC
    ${CORE}/src/nav.cpp      ${CORE}/src/shift.cpp   ${CORE}/src/geo.cpp
    ${CORE}/src/lap.cpp      ${CORE}/src/state.cpp   ${CORE}/src/units.cpp
    ${CORE}/src/sectors.cpp
    ${CORE}/src/trip.cpp     ${CORE}/src/config.cpp  ${CORE}/src/health.cpp
    ${CORE}/src/attitude.cpp ${CORE}/src/jump.cpp    ${CORE}/src/nmea.cpp
    ${CORE}/src/input.cpp    ${CORE}/src/tach.cpp    ${CORE}/src/log.cpp    ${CORE}/src/detail.cpp
    ${CORE}/src/delta.cpp ${CORE}/src/gear.cpp ${CORE}/src/power.cpp ${CORE}/src/sun.cpp ${CORE}/src/alert.cpp
    ${CORE}/src/media.cpp ${CORE}/src/pairing.cpp ${CORE}/src/ota.cpp
    ${CORE}/src/wifi.cpp
    ${CORE}/src/fusion_speed.cpp ${CORE}/src/gearcal.cpp ${CORE}/src/c6.cpp
    ${CORE}/src/fixtime.cpp
    ${CORE}/src/sweep.cpp ${CORE}/src/battery.cpp ${CORE}/src/slip.cpp ${CORE}/src/waypoint.cpp ${CORE}/src/flash.cpp
    ${CORE}/src/track.cpp ${CORE}/src/session.cpp
    ${CORE}/src/perf.cpp ${CORE}/src/stunt.cpp ${CORE}/src/hr.cpp)
target_include_directories(core PUBLIC ${CORE}/include)

add_library(ui STATIC
    ${UI}/src/font5x7.cpp
    ${UI}/src/font_aa.cpp    ${UI}/src/framebuffer_canvas.cpp
    ${UI}/src/icons.cpp
    ${UI}/src/album_art.cpp  ${UI}/src/album_art_gojira.cpp
    ${UI}/src/widgets.cpp    ${UI}/src/app.cpp
    ${UI}/src/screens_cruise.cpp ${UI}/src/screens_track.cpp ${UI}/src/screens_perf.cpp
    ${UI}/src/screens_stat.cpp   ${UI}/src/screens_diag.cpp
    ${UI}/src/screen_config.cpp ${UI}/src/screen_gearcal.cpp
    ${UI}/src/shadow.cpp)
target_include_directories(ui PUBLIC ${UI}/include)
target_link_libraries(ui PUBLIC core)

add_library(halstub STATIC ${HAL}/src/status.cpp)
target_include_directories(halstub PUBLIC ${HAL}/include)

# The logic task's body, with no FreeRTOS in it. This is the point of the
# whole exercise: the simulator executes the firmware's decisions rather
# than a copy of them that would quietly drift.
add_library(logic STATIC ${SVC}/src/logic_tick.cpp ${SVC}/src/track_store.cpp)
target_include_directories(logic PUBLIC ${SVC}/include)
target_link_libraries(logic PUBLIC core ui halstub)

add_executable(dash_sim sim_main.cpp sim_bike.cpp)
target_link_libraries(dash_sim PRIVATE logic ui core halstub)

# What each screen costs on the wire. Not a test — a report you read when
# deciding which widget to make cheaper. See ui/wire_cost.hpp.
add_executable(wire_report wire_report.cpp)
target_link_libraries(wire_report PRIVATE ui core halstub)
