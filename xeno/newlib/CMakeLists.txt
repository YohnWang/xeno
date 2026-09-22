add_library(xeno_newlib)

target_link_libraries(xeno_newlib PUBLIC xeno_headers)
#target_link_options(xeno_newlib PUBLIC -Wl,--wrap=_free_r)
target_sources(xeno_newlib
    PRIVATE
        init.cc
        stdio.cc
        sbrk.cc
        signal.cc
        sleep.cc
        time.cc
        tls.cc
        unistd.cc
        weak.cc
)