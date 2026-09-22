add_library(xeno_thread_single)

target_sources(xeno_thread_single
    PRIVATE
        futex.cc
        nanosleep.cc
        pthread.cc
        pthread_mutex.cc
        pthread_cond.cc
        pthread_once.cc
        sched.cc
        sem.cc
        start.cc
        ../thread/pthread_attr.cc
)

target_link_libraries(xeno_thread_single PUBLIC xeno_headers freertos_headers)

target_compile_options(xeno_thread_single PUBLIC -ftls-model=local-exec)
target_link_options(xeno_thread_single PUBLIC -ftls-model=local-exec)