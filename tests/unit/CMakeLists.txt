function(picovdp_unit_test name)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE picovdp_warnings)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

picovdp_unit_test(test_placeholder)
