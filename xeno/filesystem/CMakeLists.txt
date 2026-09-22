add_library(xeno_filesystem)

target_include_directories(xeno_filesystem INTERFACE .)
target_link_libraries(xeno_filesystem PUBLIC xeno_headers)
target_sources(xeno_filesystem PRIVATE console.cc filesystem.cc)
target_compile_options(xeno_filesystem PUBLIC $<$<COMPILE_LANGUAGE:CXX>:-freflection>)