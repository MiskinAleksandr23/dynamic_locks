function(add_catch TARGET)
    add_executable(${TARGET} ${ARGN})
    target_link_libraries(${TARGET} contrib_catch_main)
endfunction()
