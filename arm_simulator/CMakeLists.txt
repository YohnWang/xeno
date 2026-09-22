project(arm-simulator)
enable_language(C CXX ASM)

function(add_linkerscript target path)
    cmake_path(IS_RELATIVE path path_is_relative)
    if(path_is_relative)
        set(source_path ${CMAKE_CURRENT_LIST_DIR}/${path})
    else()
        set(source_path ${path})
    endif()
    cmake_path(GET source_path PARENT_PATH parent)

    add_library(${target} INTERFACE)
    target_link_options(${target} INTERFACE -T ${source_path})
    target_link_directories(${target} INTERFACE ${parent})
    set_target_properties(${target} PROPERTIES INTERFACE_LINK_DEPENDS ${source_path})
endfunction()

add_subdirectory(semihost)
add_subdirectory(device)
add_subdirectory(xeno_port)

add_executable(main main.cc)

target_link_libraries(main PRIVATE device semihost  xeno $<LINK_LIBRARY:WHOLE_ARCHIVE,xeno_thread,xeno_port>)

add_custom_command(TARGET main POST_BUILD
    COMMAND size main -A
    COMMAND ${CMAKE_OBJDUMP} -d main > main.s
    COMMAND ${CMAKE_OBJCOPY} -O binary -S main main.bin
    COMMAND ls -lh main.bin
)

target_link_options(main PRIVATE -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/main.map,--cref)

add_custom_target(sim
    COMMAND time qemu-system-arm 
    -M mps2-an500
    -kernel $<TARGET_FILE:main>
    -semihosting-config enable=on,target=native,chardev=stdio 
    -chardev stdio,id=stdio,mux=on,signal=off 
    -serial null -icount shift=auto
    -no-reboot
    COMMAND echo exit code: $$?
    DEPENDS main USES_TERMINAL
)