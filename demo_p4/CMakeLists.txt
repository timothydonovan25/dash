cmake_minimum_required(VERSION 3.16)

# The real dash's core/ui/dash_hal live one level up. This project borrows
# them unmodified rather than forking them — see docs/bringup-p4-demo.md.
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Named "dash", like the real firmware, because the stable/dev slot code
# refuses any other project name (core/ota check_header, tools/flash_dev.py)
# and testing that code on this board is what this build is for.
project(dash)
