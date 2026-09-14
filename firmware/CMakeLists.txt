# The firmware (PLAN.md section 3): pico, pro-debug and pro-release presets.
add_executable(picovdp
    main.c
    renderer.c
    fault.c
    vga/vga.c
    pio-utils/pio_utils.c)

set_target_properties(picovdp PROPERTIES SUFFIX .elf)

pico_set_program_name(picovdp "6502-PICOVDP")
pico_set_program_description(picovdp "6502-PICOVDP video card firmware")
pico_set_binary_type(picovdp copy_to_ram)
pico_generate_pio_header(picovdp ${CMAKE_CURRENT_LIST_DIR}/vga/vga.pio)

execute_process(
    COMMAND git describe --always --dirty --exclude "*"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE PICOVDP_BUILD_ID
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT PICOVDP_BUILD_ID)
    set(PICOVDP_BUILD_ID "uncommitted")
endif()

target_include_directories(picovdp PRIVATE ${CMAKE_CURRENT_LIST_DIR} vga pio-utils)
target_compile_definitions(picovdp PRIVATE
    PICOVDP_DEBUG=$<BOOL:${PICOVDP_DEBUG}>
    PICOVDP_BUILD_ID="${PICOVDP_BUILD_ID}"
    PICO_BOARD="${PICO_BOARD}"
    # A panic writes a fault record and reboots (fault.h).
    PICO_PANIC_FUNCTION=picovdp_panic
    # Flash is never touched after boot (copy_to_ram), but keep its clock in
    # range at 352 MHz, as pico9918 does.
    PICO_FLASH_SPI_CLKDIV=4)

target_link_libraries(picovdp PRIVATE
    picovdp_warnings
    picovdp_core
    pico_stdlib
    pico_multicore
    pico_unique_id
    hardware_dma
    hardware_pio)

if(PICOVDP_DEBUG)
    # The debug link runs over USB CDC; pico_stdio_usb's reset interface is what
    # lets `picotool load -x -f` reboot a running board into BOOTSEL.
    target_sources(picovdp PRIVATE link.c inject.c profile.c scenes.c)
    target_link_libraries(picovdp PRIVATE picovdp_core_debug hardware_pwm)
    pico_enable_stdio_usb(picovdp 1)
else()
    pico_enable_stdio_usb(picovdp 0)
endif()
pico_enable_stdio_uart(picovdp 0)

pico_add_extra_outputs(picovdp)
