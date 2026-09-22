add_library(xeno_thread)

target_link_libraries(xeno_thread PUBLIC xeno_headers freertos_kernel xeno_filesystem)

target_sources(xeno_thread
    PRIVATE
        common.h
        futex.cc
        lock.cc
        nanosleep.cc
        pthread_attr.cc
        pthread_cond.cc
        pthread_key.cc
        pthread_mutex.cc
        pthread_once.cc
        pthread.cc
        sched.cc
        sem.cc
        time.cc
)

# __lock
target_link_options(xeno_thread INTERFACE -Wl,--wrap=__lock___sinit_recursive_mutex)
target_link_options(xeno_thread INTERFACE -Wl,--wrap=__lock___sfp_recursive_mutex)
target_link_options(xeno_thread INTERFACE -Wl,--wrap=__lock___atexit_recursive_mutex)
target_link_options(xeno_thread INTERFACE -Wl,--wrap=__lock___at_quick_exit_mutex)
target_link_options(xeno_thread INTERFACE -Wl,--wrap=__lock___malloc_recursive_mutex)
target_link_options(xeno_thread INTERFACE -Wl,--wrap=__lock___env_recursive_mutex)
target_link_options(xeno_thread INTERFACE -Wl,--wrap=__lock___tz_mutex)
target_link_options(xeno_thread INTERFACE -Wl,--wrap=__lock___dd_hash_mutex)
target_link_options(xeno_thread INTERFACE -Wl,--wrap=__lock___arc4random_mutex)
target_link_options(xeno_thread INTERFACE -Wl,--wrap=__retarget_lock_init)
target_link_options(xeno_thread INTERFACE -Wl,--wrap=__retarget_lock_init_recursive)
target_link_options(xeno_thread INTERFACE -Wl,--wrap=__retarget_lock_close)
target_link_options(xeno_thread INTERFACE -Wl,--wrap=__retarget_lock_close_recursive)
target_link_options(xeno_thread INTERFACE -Wl,--wrap=__retarget_lock_acquire)
target_link_options(xeno_thread INTERFACE -Wl,--wrap=__retarget_lock_acquire_recursive)
target_link_options(xeno_thread INTERFACE -Wl,--wrap=__retarget_lock_try_acquire)
target_link_options(xeno_thread INTERFACE -Wl,--wrap=__retarget_lock_try_acquire_recursive)
target_link_options(xeno_thread INTERFACE -Wl,--wrap=__retarget_lock_release)
target_link_options(xeno_thread INTERFACE -Wl,--wrap=__retarget_lock_release_recursive)