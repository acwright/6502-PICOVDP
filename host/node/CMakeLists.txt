# The core as a Node-API addon, for host/node/Video.cjs (PLAN.md section 4).
#
# Built with CMake against the headers of the Node on PATH, rather than with
# node-gyp: nothing to install, and the core is compiled by the same toolchain
# and flags as everything else on the host. Node-API is ABI-stable, so the
# addon loads in any Node of the same major API version.

find_program(PICOVDP_NODE node)
if(NOT PICOVDP_NODE)
    message(STATUS "host/node: no node on PATH, the addon is not built")
    return()
endif()

execute_process(
    COMMAND ${PICOVDP_NODE} -p "require('path').resolve(process.execPath, '..', '..', 'include', 'node')"
    OUTPUT_VARIABLE PICOVDP_NODE_INCLUDE
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT EXISTS ${PICOVDP_NODE_INCLUDE}/node_api.h)
    message(STATUS "host/node: no node_api.h beside ${PICOVDP_NODE}, the addon is not built")
    return()
endif()

add_library(picovdp_node MODULE addon.c)
target_include_directories(picovdp_node SYSTEM PRIVATE ${PICOVDP_NODE_INCLUDE})
target_compile_definitions(picovdp_node PRIVATE NODE_GYP_MODULE_NAME=picovdp)
target_link_libraries(picovdp_node PRIVATE picovdp_core_debug picovdp_warnings)
set_target_properties(picovdp_node PROPERTIES
    PREFIX ""
    SUFFIX ".node"
    OUTPUT_NAME picovdp
    # Where Video.cjs looks for it by default.
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/host/node)
if(APPLE)
    # Node-API symbols are resolved against the node binary that loads it.
    target_link_options(picovdp_node PRIVATE -undefined dynamic_lookup)
endif()

# Loads the addon and checks the binding is whole.
add_test(NAME node_addon_loads
    COMMAND ${PICOVDP_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/smoke.cjs $<TARGET_FILE:picovdp_node>)

# The host executor (PLAN.md section 4): every fixture, replayed from
# tests/oracle through the addon. Needs no emulator.
foreach(fixture bios wizardslab vdp-modes vdp-layers)
    add_test(NAME replay_${fixture}
        COMMAND ${PICOVDP_NODE} ${CMAKE_SOURCE_DIR}/tools/replay.mjs ${fixture} --classes)
    set_tests_properties(replay_${fixture} PROPERTIES
        ENVIRONMENT "PICOVDP_NODE_BINARY=$<TARGET_FILE:picovdp_node>")
endforeach()
