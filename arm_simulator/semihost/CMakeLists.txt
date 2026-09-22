add_library(semihost)

target_include_directories(semihost PUBLIC .)
target_sources(semihost PRIVATE semihost-arm.S)