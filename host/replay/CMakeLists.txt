# vdp-replay: the host executor in pure C (PLAN.md section 4). A trace from
# tests/oracle into the core, each checkpoint against its golden. Needs zlib,
# which macOS has, and nothing of Node or the emulator.

find_package(ZLIB REQUIRED)

add_executable(vdp-replay replay.c)
target_link_libraries(vdp-replay PRIVATE picovdp_core_debug picovdp_warnings ZLIB::ZLIB)

foreach(fixture bios wizardslab vdp-modes vdp-layers)
    add_test(NAME vdp_replay_${fixture}
        COMMAND vdp-replay --classes ${CMAKE_SOURCE_DIR}/tests/oracle/${fixture}/${fixture}.vdpt.gz)
endforeach()
