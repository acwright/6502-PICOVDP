# The portable core (PLAN.md section 3): no pico-sdk, no hardware (rule 5).
# Built on both platforms from the start, so the RP2350 toolchain keeps it honest.
add_library(picovdp_core STATIC vdp.c)
target_include_directories(picovdp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(picovdp_core PRIVATE picovdp_warnings)
if(PICOVDP_PLATFORM STREQUAL "host")
    # Linked into the Node-API addon, a shared module.
    set_target_properties(picovdp_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
