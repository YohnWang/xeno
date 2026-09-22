project(xeno)

add_compile_options(-ftls-model=local-exec)
add_link_options(-ftls-model=local-exec)

set($CACHE{XENO_HEADERS_PATH} TYPE PATH FORCE VALUE ${CMAKE_CURRENT_LIST_DIR}/include)
add_library(xeno_headers INTERFACE)
target_include_directories(xeno_headers INTERFACE include)
target_link_libraries(xeno_headers INTERFACE freertos_headers)
target_compile_options(xeno_headers
    INTERFACE 
        $<$<COMPILE_LANGUAGE:C,CXX>:--include=xeno/predefined.h>
        -ftls-model=local-exec
)

add_subdirectory(freertos)

add_subdirectory(backtrace)
add_subdirectory(c++)
add_subdirectory(filesystem)
add_subdirectory(newlib)
add_subdirectory(thread)
add_subdirectory(thread_single)

add_library(xeno INTERFACE)
target_link_libraries(xeno INTERFACE xeno_headers $<LINK_LIBRARY:WHOLE_ARCHIVE,xeno_filesystem,xeno_newlib,xeno_c++>)

target_link_options(xeno INTERFACE -nostartfiles)
