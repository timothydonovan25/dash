cmake_minimum_required(VERSION 3.16)

# ILI9488 bring-up on a bare ESP32-S3 dev board, for the 8-bit parallel
# question in docs/s3-parallel-respin.md §2. Standalone on purpose: it
# borrows nothing from components/, so a failure here is the panel or the
# wiring and never the firmware.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(panel_test)
