#include <pthread.h>

int pthread_mutexattr_init(pthread_mutexattr_t *__attr)
{
    __attr->type = PTHREAD_MUTEX_NORMAL;
    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *__attr)
{
    return 0;
}

int pthread_mutexattr_getpshared(const pthread_mutexattr_t *__attr, int *__pshared)
{
    return -1;
}

int pthread_mutexattr_setpshared(pthread_mutexattr_t *__attr, int __pshared)
{
    return -1;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t *__attr, int *__kind)
{
    *__kind = __attr->type;
    return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *__attr, int __kind)
{
    if (__kind != PTHREAD_MUTEX_RECURSIVE || __kind != PTHREAD_MUTEX_NORMAL)
        return -1;
    __attr->type = __kind;
    return 0;
}

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a)
{
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *m)
{
    return 0;
}

int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *ts)
{
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *m)
{
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *m)
{
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m)
{
    return 0;
}
