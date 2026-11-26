add_library(xeno_syscmd)

target_link_libraries(xeno_syscmd PUBLIC xeno_headers xeno_toolkit)
target_sources(xeno_syscmd PRIVATE syscmd.cc)
