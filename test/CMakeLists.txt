# Host build. Deliberately does NOT use idf.py — this compiles
# components/core with plain g++ so the logic can be tested in a second
# instead of a flash cycle. Nothing here may include an ESP header.
cmake_minimum_required(VERSION 3.16)
project(dash_host_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Wpedantic)

enable_testing()

set(CORE ${CMAKE_CURRENT_SOURCE_DIR}/../components/core)

add_library(core STATIC
    ${CORE}/src/nav.cpp
    ${CORE}/src/shift.cpp
    ${CORE}/src/geo.cpp
    ${CORE}/src/lap.cpp
    ${CORE}/src/sectors.cpp
    ${CORE}/src/state.cpp
    ${CORE}/src/units.cpp
    ${CORE}/src/trip.cpp
    ${CORE}/src/config.cpp
    ${CORE}/src/health.cpp
    ${CORE}/src/attitude.cpp
    ${CORE}/src/jump.cpp
    ${CORE}/src/nmea.cpp
    ${CORE}/src/input.cpp
    ${CORE}/src/tach.cpp
    ${CORE}/src/log.cpp
    ${CORE}/src/detail.cpp
    ${CORE}/src/delta.cpp
    ${CORE}/src/gear.cpp
    ${CORE}/src/power.cpp
    ${CORE}/src/sun.cpp
    ${CORE}/src/alert.cpp
    ${CORE}/src/media.cpp
    ${CORE}/src/pairing.cpp
    ${CORE}/src/ota.cpp
    ${CORE}/src/wifi.cpp
    ${CORE}/src/sweep.cpp
    ${CORE}/src/fusion_speed.cpp
    ${CORE}/src/fixtime.cpp
    ${CORE}/src/gearcal.cpp
    ${CORE}/src/c6.cpp
    ${CORE}/src/battery.cpp
    ${CORE}/src/slip.cpp
    ${CORE}/src/waypoint.cpp
    ${CORE}/src/flash.cpp
    ${CORE}/src/track.cpp
    ${CORE}/src/session.cpp
    ${CORE}/src/perf.cpp
    ${CORE}/src/stunt.cpp
    ${CORE}/src/hr.cpp)
target_include_directories(core PUBLIC ${CORE}/include)

# The UI builds on the host too: a Canvas is an interface, and nothing in
# ui/ includes an ESP header. That is what makes "does the lap screen show
# the right time" a unit test instead of a ride.
set(UI ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui)
add_library(ui STATIC
    ${UI}/src/font5x7.cpp
    ${UI}/src/font_aa.cpp
    ${UI}/src/framebuffer_canvas.cpp
    ${UI}/src/icons.cpp
    ${UI}/src/album_art.cpp
    ${UI}/src/album_art_gojira.cpp
    ${UI}/src/widgets.cpp
    ${UI}/src/screens_cruise.cpp
    ${UI}/src/screens_track.cpp
    ${UI}/src/screens_perf.cpp
    ${UI}/src/screens_stat.cpp
    ${UI}/src/screens_diag.cpp
    ${UI}/src/screen_config.cpp
    ${UI}/src/screen_gearcal.cpp
    ${UI}/src/app.cpp
    ${UI}/src/shadow.cpp)
target_include_directories(ui PUBLIC ${UI}/include)
target_link_libraries(ui PUBLIC core)

set(HAL ${CMAKE_CURRENT_SOURCE_DIR}/../components/dash_hal)
add_library(hal STATIC ${HAL}/src/status.cpp)
target_include_directories(hal PUBLIC ${HAL}/include)

foreach(t nav shift geo published lap state units trip config health
          attitude jump nmea input tach log detail delta gear gearcal c6 power media pairing ota sweep wifi sun alert battery slip waypoint flash wire ui shadow fixtime sectors track session perf stunt hr)
    add_executable(test_${t} test_${t}.cpp)
    target_link_libraries(test_${t} PRIVATE core)
    target_include_directories(test_${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${t} COMMAND test_${t})
endforeach()

# The settings half of `keeper`: board-free, so it is tested here.
set(SVC ${CMAKE_CURRENT_SOURCE_DIR}/../components/services)
add_executable(test_settings_store test_settings_store.cpp ${SVC}/src/settings_store.cpp)
target_include_directories(test_settings_store PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${SVC}/include)
target_link_libraries(test_settings_store PRIVATE core hal)
add_test(NAME settings_store COMMAND test_settings_store)

# `logic`'s half of the gear calibration, through the real logic_tick.
add_executable(test_logic_gearcal test_logic_gearcal.cpp ${SVC}/src/logic_tick.cpp ${SVC}/src/track_store.cpp)
target_include_directories(test_logic_gearcal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${SVC}/include)
target_link_libraries(test_logic_gearcal PRIVATE core ui hal)
add_test(NAME logic_gearcal COMMAND test_logic_gearcal)

# `logic`'s half of the lap timer: fixes at 10 Hz through a 50 Hz tick.
add_executable(test_logic_lap test_logic_lap.cpp ${SVC}/src/logic_tick.cpp ${SVC}/src/track_store.cpp)
target_include_directories(test_logic_lap PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${SVC}/include)
target_link_libraries(test_logic_lap PRIVATE core ui hal)
add_test(NAME logic_lap COMMAND test_logic_lap)

# `logic`'s half of the named waypoints.
add_executable(test_logic_waypoint test_logic_waypoint.cpp ${SVC}/src/logic_tick.cpp ${SVC}/src/track_store.cpp)
target_include_directories(test_logic_waypoint PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${SVC}/include)
target_link_libraries(test_logic_waypoint PRIVATE core ui hal)
add_test(NAME logic_waypoint COMMAND test_logic_waypoint)

# `logic`'s half of saved tracks, served by the real track_store against a
# card in RAM.
add_executable(test_logic_track test_logic_track.cpp ${SVC}/src/logic_tick.cpp ${SVC}/src/track_store.cpp)
target_include_directories(test_logic_track PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${SVC}/include)
target_link_libraries(test_logic_track PRIVATE core ui hal)
add_test(NAME logic_track COMMAND test_logic_track)

foreach(t ui wire shadow)
    target_link_libraries(test_${t} PRIVATE ui hal)
endforeach()

# Rule 3, enforced rather than remembered: core, ui and hal may never include
# an ESP header. Breaking it is a one-line accident in a commit about
# something else, and the symptom is that this whole build stops working.
add_test(NAME no_platform_headers
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/check_no_esp_headers.sh
                 ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Sensor fusion: the tests ride the synthetic scenarios that tools/replay
# also runs, so a number seen in `replay --scenario all` is the one tested.
set(REPLAY ${CMAKE_CURRENT_SOURCE_DIR}/../tools/replay)
add_executable(test_fusion test_fusion.cpp)
target_link_libraries(test_fusion PRIVATE core)
target_include_directories(test_fusion PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${REPLAY})
add_test(NAME fusion COMMAND test_fusion)

add_executable(replay ${REPLAY}/replay.cpp)
target_link_libraries(replay PRIVATE core)
