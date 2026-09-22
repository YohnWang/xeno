#include <pthread.h>

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr)
{
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond)
{
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond)
{
    return 0;
}

int pthread_cond_signal(pthread_cond_t *cond)
{
    return 0;
}

int pthread_cond_clockwait(pthread_cond_t *__restrict cond,
                           pthread_mutex_t *__restrict mutex, clockid_t clockid,
                           const struct timespec *__restrict abstime)
{
    return 0;
}

int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abstime)
{
    return 0;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    return 0;
}
