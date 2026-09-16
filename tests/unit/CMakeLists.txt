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
picovdp_unit_test(test_font       # §7's built-in font, §14, §15
    ${CMAKE_SOURCE_DIR}/fonts/cp437-6x8.bin)
picovdp_unit_test(test_status)    # §3's line numbering, §6, §14
picovdp_unit_test(test_tiles)     # §8 at 1bpp, §9, the backdrop and border
picovdp_unit_test(test_layers)    # §8 at 2, 4 and 8bpp, §12's seven levels, §13 at depth
picovdp_unit_test(test_sprites)   # §10, §9's legacy sprites, §12 against the layers, the cores' split

# Phase 8's word renderer against Phase 7's pixel renderer, kept as the reference.
add_executable(test_render test_render.c reference.c)
target_link_libraries(test_render PRIVATE picovdp_core_debug picovdp_warnings)
add_test(NAME test_render COMMAND test_render)
