# C unit tests, one executable per concern, by SPEC section. They reach the
# core through vdp.h and vdp_debug.h, and look inside vdp_t where a test is
# about the render side, which no bus operation can see.
function(picovdp_unit_test name)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE picovdp_core_debug picovdp_warnings)
    add_test(NAME ${name} COMMAND ${name} ${ARGN})
endfunction()

picovdp_unit_test(test_bus)       # §4, §5, §7
picovdp_unit_test(test_latch)     # §3's latch, the journal and its overflow
picovdp_unit_test(test_palette    # §11
    ${CMAKE_SOURCE_DIR}/SPEC.md ${CMAKE_SOURCE_DIR}/picovdp-default.pal)
picovdp_unit_test(test_reset)     # §15
picovdp_unit_test(test_status)    # §3's line numbering, §6, §14
