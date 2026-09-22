add_library(xeno_c++)
add_library(xeno_cxx ALIAS xeno_c++)

target_link_libraries(xeno_c++ PUBLIC xeno_headers)

target_link_options(xeno_c++ INTERFACE -Wl,--wrap=__cxa_guard_acquire -Wl,--wrap=__cxa_guard_release -Wl,--wrap=__cxa_guard_abort)
target_link_options(xeno_c++ INTERFACE -Wl,--wrap=__cxa_allocate_exception)

target_sources(xeno_c++
    PRIVATE
        atexit_thread.cc
        atomic.cc
        chrono.cc
        condition_variable.cc
        cxa_guard.cc
        eh_alloc.cc
        dso_handle.cc
        future.cc
        mutex.cc
        thread.cc
        vterminate.cc
)
