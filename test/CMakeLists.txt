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
    ${CORE}/src/delta.cpp
    ${CORE}/src/gear.cpp)
target_include_directories(core PUBLIC ${CORE}/include)

# The UI builds on the host too: a Canvas is an interface, and nothing in
# ui/ includes an ESP header. That is what makes "does the lap screen show
# the right time" a unit test instead of a ride.
set(UI ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui)
add_library(ui STATIC
    ${UI}/src/font5x7.cpp
    ${UI}/src/framebuffer_canvas.cpp
    ${UI}/src/icons.cpp
    ${UI}/src/widgets.cpp
    ${UI}/src/screens_cruise.cpp
    ${UI}/src/screens_track.cpp
    ${UI}/src/screens_stat.cpp
    ${UI}/src/screens_diag.cpp
    ${UI}/src/screen_config.cpp
    ${UI}/src/app.cpp)
target_include_directories(ui PUBLIC ${UI}/include)
target_link_libraries(ui PUBLIC core)

set(HAL ${CMAKE_CURRENT_SOURCE_DIR}/../components/dash_hal)
add_library(hal STATIC ${HAL}/src/status.cpp)
target_include_directories(hal PUBLIC ${HAL}/include)

foreach(t nav shift geo published lap state units trip config health
          attitude jump nmea input tach log delta gear ui)
    add_executable(test_${t} test_${t}.cpp)
    target_link_libraries(test_${t} PRIVATE core)
    target_include_directories(test_${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${t} COMMAND test_${t})
endforeach()

foreach(t ui)
    target_link_libraries(test_${t} PRIVATE ui hal)
endforeach()

# Rule 3, enforced rather than remembered: core, ui and hal may never include
# an ESP header. Breaking it is a one-line accident in a commit about
# something else, and the symptom is that this whole build stops working.
add_test(NAME no_platform_headers
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/check_no_esp_headers.sh
                 ${CMAKE_CURRENT_SOURCE_DIR}/..)
