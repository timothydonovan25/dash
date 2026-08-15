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
    ${CORE}/src/geo.cpp)
target_include_directories(core PUBLIC ${CORE}/include)

foreach(t nav shift geo published)
    add_executable(test_${t} test_${t}.cpp)
    target_link_libraries(test_${t} PRIVATE core)
    target_include_directories(test_${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${t} COMMAND test_${t})
endforeach()
