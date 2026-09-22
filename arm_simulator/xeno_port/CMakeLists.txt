add_library(xeno_port)
target_link_libraries(xeno_port PUBLIC semihost xeno_headers)
target_sources(xeno_port
    PRIVATE
        malloc.cc
        stdio.cc
        exit.cc
        time.cc
)