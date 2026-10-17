cmake_minimum_required(VERSION 3.16)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/dash_version.cmake)
dash_firmware_version(bike)   # PROJECT_VER, from firmware.txt

include(${CMAKE_CURRENT_LIST_DIR}/cmake/dash_stack.cmake)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dash)
dash_firmware_outputs()      # build date, and build/dash-<version>-...bin
dash_frame_limit()           # -Wframe-larger-than on our components (cmake/dash_stack.cmake)
