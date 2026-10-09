# Sets PROJECT_VER from firmware.txt, before project(), for either board:
#
#     include(${CMAKE_CURRENT_LIST_DIR}/cmake/dash_version.cmake)
#     dash_firmware_version(bike)
#
# PROJECT_VER becomes "<version> <board> <feature>", which IDF puts in the
# image's esp_app_desc_t (31 characters at most) -- so the version, board and
# feature travel inside the image, where the Update screen and core::ota's
# board check read them. After project(), dash_firmware_outputs() keeps the
# image's date current and names the .bin.

set(DASH_FIRMWARE_TXT "${CMAKE_CURRENT_LIST_DIR}/../firmware.txt")

function(dash_firmware_version board)
    file(STRINGS "${DASH_FIRMWARE_TXT}" lines REGEX "^(version|feature) ")
    set(version "")
    set(feature "")
    foreach(line IN LISTS lines)
        if(line MATCHES "^version +(.+)$")
            set(version "${CMAKE_MATCH_1}")
        elseif(line MATCHES "^feature +(.+)$")
            set(feature "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    if(NOT version MATCHES "^[0-9]+\\.[0-9]+(\\.[0-9]+)?$")
        message(FATAL_ERROR "firmware.txt: version must look like 1.0, got '${version}'")
    endif()
    string(REGEX MATCHALL "[^ ]+" words "${feature}")
    list(LENGTH words n)
    if(n LESS 1 OR n GREATER 3)
        message(FATAL_ERROR "firmware.txt: feature is one to three words, got '${feature}'")
    endif()
    set(ver "${version} ${board} ${feature}")
    string(LENGTH "${ver}" len)
    if(len GREATER 31)
        message(FATAL_ERROR "firmware.txt: '${ver}' is ${len} characters; the image "
                            "holds 31. Shorten the feature.")
    endif()
    set(PROJECT_VER "${ver}" PARENT_SCOPE)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DASH_FIRMWARE_TXT}")
endfunction()

function(dash_firmware_outputs)
    # IDF stamps the image with __DATE__ from esp_app_desc.c, which only
    # recompiles when the version string changes -- so a build days later
    # would still say the old date. A stamp that changes once a day, as a
    # dependency of that one file, makes it recompile on the first build of
    # each day and never otherwise.
    set(stamp "${CMAKE_BINARY_DIR}/dash_build_date.txt")
    add_custom_target(dash_build_date ALL
        COMMAND ${CMAKE_COMMAND} -DOUT=${stamp}
                -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/dash_date_stamp.cmake"
        BYPRODUCTS "${stamp}")
    idf_component_get_property(desc_dir esp_app_format COMPONENT_DIR)
    idf_component_get_property(desc_lib esp_app_format COMPONENT_LIB)
    set_source_files_properties("${desc_dir}/esp_app_desc.c"
        TARGET_DIRECTORY ${desc_lib}
        PROPERTIES OBJECT_DEPENDS "${stamp}")
    add_dependencies(${desc_lib} dash_build_date)

    # The named copy, from what is actually inside the image.
    idf_build_get_property(build_dir BUILD_DIR)
    idf_build_get_property(name PROJECT_NAME)
    add_custom_command(TARGET app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -DBIN=${build_dir}/${name}.bin -DOUTDIR=${build_dir}
                -DRELEASES=${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../releases
                -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/dash_name_bin.cmake"
        VERBATIM)
endfunction()
