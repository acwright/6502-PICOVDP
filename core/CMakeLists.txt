# The portable core (PLAN.md section 3): no pico-sdk, no hardware (rule 5).
# Built on both platforms from the start, so the RP2350 toolchain keeps it honest.
add_library(picovdp_core STATIC bus.c geometry.c line.c palette.c render.c status.c)
target_include_directories(picovdp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(picovdp_core PRIVATE picovdp_warnings)

# Inspection and snapshots (vdp_debug.h): the adapter, the unit tests and the
# debug link. Kept out of picovdp_core so a release image need not link it.
add_library(picovdp_core_debug STATIC debug.c)
target_link_libraries(picovdp_core_debug PUBLIC picovdp_core PRIVATE picovdp_warnings)

if(PICOVDP_PLATFORM STREQUAL "host")
    # Linked into the Node-API addon, a shared module.
    set_target_properties(picovdp_core picovdp_core_debug PROPERTIES POSITION_INDEPENDENT_CODE ON)
else()
    target_compile_definitions(picovdp_core PRIVATE PICOVDP_RP2350)
endif()
