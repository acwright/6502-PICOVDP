# Phase 1 timing spike (PLAN.md section 6). Disposable.
#   host:   spike_host, the renderer checked against a per-pixel reference (CTest)
#   rp2350: picovdp_spike, the renderer timed on core 1 at both clock presets

set(PICOVDP_SPIKE_OPT "" CACHE STRING "Optimisation flag for the timed spike code (empty: the preset's)")

if(PICOVDP_PLATFORM STREQUAL "host")
    add_executable(spike_host host_main.c render.c scenes.c)
    target_link_libraries(spike_host PRIVATE picovdp_warnings)
    add_test(NAME spike_reference COMMAND spike_host)
else()
    add_executable(picovdp_spike pico_main.c render.c scenes.c)
    set_target_properties(picovdp_spike PROPERTIES SUFFIX .elf)
    pico_set_program_name(picovdp_spike "6502-PICOVDP Phase 1 spike")
    pico_set_binary_type(picovdp_spike copy_to_ram)
    if(PICOVDP_SPIKE_OPT)
        set_source_files_properties(render.c PROPERTIES COMPILE_OPTIONS ${PICOVDP_SPIKE_OPT})
    endif()
    target_compile_definitions(picovdp_spike PRIVATE
        PICOVDP_SPIKE_OPT="${PICOVDP_SPIKE_OPT}"
        # Flash is never touched after boot (copy_to_ram), but keep its clock
        # in range at 352 MHz, as pico9918 does.
        PICO_FLASH_SPI_CLKDIV=4)
    target_link_libraries(picovdp_spike PRIVATE picovdp_warnings pico_stdlib pico_multicore hardware_pio)
    pico_enable_stdio_usb(picovdp_spike 1)
    pico_enable_stdio_uart(picovdp_spike 0)
    pico_add_extra_outputs(picovdp_spike)
endif()
