add_executable(picovdp main.c)

set_target_properties(picovdp PROPERTIES SUFFIX .elf)

pico_set_program_name(picovdp "6502-PICOVDP")
pico_set_program_description(picovdp "6502-PICOVDP video card firmware")
pico_set_binary_type(picovdp copy_to_ram)

execute_process(
    COMMAND git describe --always --dirty --exclude "*"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE PICOVDP_BUILD_ID
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT PICOVDP_BUILD_ID)
    set(PICOVDP_BUILD_ID "uncommitted")
endif()

target_compile_definitions(picovdp PRIVATE
    PICOVDP_DEBUG=$<BOOL:${PICOVDP_DEBUG}>
    PICOVDP_BUILD_ID="${PICOVDP_BUILD_ID}"
    PICO_BOARD="${PICO_BOARD}")

target_link_libraries(picovdp PRIVATE picovdp_warnings pico_stdlib pico_unique_id)

# The debug link runs over USB CDC; pico_stdio_usb's reset interface is what
# lets `picotool load -x -f` reboot a running board into BOOTSEL.
if(PICOVDP_DEBUG)
    pico_enable_stdio_usb(picovdp 1)
else()
    pico_enable_stdio_usb(picovdp 0)
endif()
pico_enable_stdio_uart(picovdp 0)

pico_add_extra_outputs(picovdp)
