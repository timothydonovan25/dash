# The stack-frame limit: GCC's -Wframe-larger-than on this repo's own
# components, never on ESP-IDF's. A task stack here is 3-8 kB, and a big
# object made a local is invisible to the host tests (a PC stack is
# megabytes) and fits the build until it overflows a task at run time: 1.26
# put LogicState (5.8 kB) on the demo's 4 kB bike task, and the demo booted
# back into its stable image. The firmware builds with -Werror, so a frame
# over the limit is a failed build. A function that needs more says so
# with DASH_BIG_FRAME (bsp/stack.hpp), where the reason can be read.
#
# Call after project(), when the component targets exist.
set(DASH_FRAME_LIMIT_BYTES 1024)

function(dash_frame_limit)
    get_filename_component(repo "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    idf_build_get_property(comps BUILD_COMPONENTS)
    foreach(c ${comps})
        idf_component_get_property(dir ${c} COMPONENT_DIR)
        idf_component_get_property(lib ${c} COMPONENT_LIB)
        string(FIND "${dir}" "${repo}/" at)
        string(FIND "${dir}" "/managed_components/" managed)
        if(at EQUAL 0 AND managed EQUAL -1 AND TARGET ${lib})
            get_target_property(type ${lib} TYPE)
            if(NOT type STREQUAL "INTERFACE_LIBRARY")
                target_compile_options(${lib} PRIVATE -Wframe-larger-than=${DASH_FRAME_LIMIT_BYTES})
            endif()
        endif()
    endforeach()
endfunction()
