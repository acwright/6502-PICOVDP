# vdp-scene: the firmware's worst-case scenes drawn by the core on the host, the
# reference for the firmware's snapshots of them (PLAN.md Phase 8).

add_executable(vdp-scene scene.c ${CMAKE_SOURCE_DIR}/firmware/scenes.c)
target_include_directories(vdp-scene PRIVATE ${CMAKE_SOURCE_DIR}/firmware)
target_link_libraries(vdp-scene PRIVATE picovdp_core_debug picovdp_warnings)

add_test(NAME vdp_scene_check COMMAND vdp-scene --check)
