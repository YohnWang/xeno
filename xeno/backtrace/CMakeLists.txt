add_library(xeno_backtrace)

target_include_directories(xeno_backtrace PUBLIC .)

target_sources(xeno_backtrace
    PRIVATE
        fault_handler.cc
        print_stacktrace.cc
)

target_link_libraries(xeno_backtrace PUBLIC xeno_headers)