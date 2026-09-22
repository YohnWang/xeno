#include<time.h>
#include<limits.h>
#include<pthread.h>
#include<errno.h>

#include<bits/futex.h>
#include<xeno/kits.h>

using namespace xeno;

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr)
{
    *cond = PTHREAD_COND_INITIALIZER;
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond)
{
    int val = __atomic_load_n(cond, __ATOMIC_ACQUIRE);
    __futex_wake(cond, val + 1);
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond)
{
    *cond == PTHREAD_COND_INITIALIZER;
    return 0;
}

int pthread_cond_signal(pthread_cond_t *cond)
{
    int val = __atomic_load_n(cond, __ATOMIC_ACQUIRE);
    __futex_wake_n(cond, val + 1, 1);
    return 0;
}

int pthread_cond_clockwait(pthread_cond_t *__restrict cond,
                   pthread_mutex_t *__restrict mutex, clockid_t clockid,
				   const struct timespec *__restrict abstime)
{
    int ret = 0;
    struct timespec start;
    clock_gettime(clockid, &start);
    
    int expected = __atomic_load_n(cond, __ATOMIC_ACQUIRE);

    if(int r = pthread_mutex_unlock(mutex); r != 0)
        return r;

    if(abstime == nullptr)
    {
        __futex_wait(cond, expected);
    }
    else
    {
        struct timespec diff = timespec_diff(&start, abstime);
        int64_t ms = diff.tv_sec * 1000LL + diff.tv_nsec / 1000000LL;
        if (ms < 0)
            ms = 0;
        int r = __futex_timedwait(cond, expected, ms);
        if(r != 0)
            ret = ETIMEDOUT;
    }

    pthread_mutex_lock(mutex);
    return ret;
}

int pthread_cond_timedwait(pthread_cond_t *cond,
                           pthread_mutex_t *mutex,
                           const struct timespec *abstime)
{
    return pthread_cond_clockwait(cond, mutex, CLOCK_MONOTONIC, abstime);
}

int pthread_cond_wait(pthread_cond_t *cond,
                      pthread_mutex_t *mutex)
{
    return pthread_cond_timedwait(cond, mutex, nullptr);
}
