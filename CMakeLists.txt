cmake_minimum_required(VERSION 3.21)

# Two platforms from one tree (PLAN.md section 3). Pick one with a preset:
#   host, host-asan          macOS — the core and its unit tests
#   pico2, pro-debug, pro-release   RP2350 firmware
set(PICOVDP_PLATFORM host CACHE STRING "host or rp2350")
set_property(CACHE PICOVDP_PLATFORM PROPERTY STRINGS host rp2350)

option(PICOVDP_DEBUG "Debug link, snapshot, injection and statistics (ground rule 8)" OFF)
option(PICOVDP_SANITIZE "AddressSanitizer and UBSan (host only)" OFF)

if(PICOVDP_PLATFORM STREQUAL "rp2350")
    # The SDK is pinned as a submodule at 2.1.1 (risk 11); it must be
    # initialised before project().
    set(PICO_SDK_PATH ${CMAKE_CURRENT_LIST_DIR}/external/pico-sdk)
    if(NOT EXISTS ${PICO_SDK_PATH}/pico_sdk_init.cmake)
        message(FATAL_ERROR "external/pico-sdk is missing: git submodule update --init && git -C external/pico-sdk submodule update --init lib/tinyusb")
    endif()
    list(APPEND PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/firmware/boards)
    include(${PICO_SDK_PATH}/pico_sdk_init.cmake)
elseif(NOT PICOVDP_PLATFORM STREQUAL "host")
    message(FATAL_ERROR "PICOVDP_PLATFORM must be host or rp2350, not '${PICOVDP_PLATFORM}'")
endif()

project(picovdp C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# The SDK's sources, compiled into the firmware target, use GNU extensions
# (typeof). The host builds strict C11, which keeps core/ honest (rule 5).
if(PICOVDP_PLATFORM STREQUAL "host")
    set(CMAKE_C_EXTENSIONS OFF)
else()
    set(CMAKE_C_EXTENSIONS ON)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Warnings for this repo's code only; the SDK is not held to them.
add_library(picovdp_warnings INTERFACE)
target_compile_options(picovdp_warnings INTERFACE -Wall -Wextra)
if(PICOVDP_PLATFORM STREQUAL "host")
    target_compile_options(picovdp_warnings INTERFACE -Werror)
endif()

if(PICOVDP_PLATFORM STREQUAL "host")
    if(PICOVDP_SANITIZE)
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
        add_link_options(-fsanitize=address,undefined)
    endif()
    enable_testing()
    add_subdirectory(core)
    add_subdirectory(tests/unit)
    add_subdirectory(host/replay)
    add_subdirectory(spike)
    # tests/oracle is written by tools/sync-oracle.mjs alone (rule 4), so its
    # check is registered here rather than beside it.
    find_program(PICOVDP_NODE node)
    if(PICOVDP_NODE)
        add_test(NAME oracle_pinned
            COMMAND ${PICOVDP_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/sync-oracle.mjs --check)
    endif()
    # An addon built with ASan cannot be loaded by a Node that was not, so the
    # adapter is built by the plain host preset only.
    if(NOT PICOVDP_SANITIZE)
        add_subdirectory(host/node)
    endif()
else()
    pico_sdk_init()
    add_subdirectory(core)
    add_subdirectory(firmware)
    add_subdirectory(spike)
endif()
