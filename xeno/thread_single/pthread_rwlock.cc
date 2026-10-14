// 单线程构建：没有别的任务，锁永远拿得到（和这里的互斥量桩一致）
#include <pthread.h>
#include <errno.h>

int pthread_rwlock_init(pthread_rwlock_t *__rwlock, const pthread_rwlockattr_t *__attr)
{
    return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *__rwlock)
{
    return 0;
}

int pthread_rwlockattr_init(pthread_rwlockattr_t *__attr)
{
    __attr->is_initialized = 1;
    return 0;
}

int pthread_rwlockattr_destroy(pthread_rwlockattr_t *__attr)
{
    __attr->is_initialized = 0;
    return 0;
}

int pthread_rwlockattr_getpshared(const pthread_rwlockattr_t *__attr, int *__pshared)
{
    return -1;
}

int pthread_rwlockattr_setpshared(pthread_rwlockattr_t *__attr, int __pshared)
{
    return -1;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *__rwlock)
{
    return 0;
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *__rwlock)
{
    return 0;
}

int pthread_rwlock_timedrdlock(pthread_rwlock_t *__restrict __rwlock,
                               const struct timespec *__restrict __abstime)
{
    return 0;
}

int pthread_rwlock_clockrdlock(pthread_rwlock_t *__restrict __rwlock,
                               clockid_t __clockid,
                               const struct timespec *__restrict __abstime)
{
    return 0;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *__rwlock)
{
    return 0;
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *__rwlock)
{
    return 0;
}

int pthread_rwlock_timedwrlock(pthread_rwlock_t *__restrict __rwlock,
                               const struct timespec *__restrict __abstime)
{
    return 0;
}

int pthread_rwlock_clockwrlock(pthread_rwlock_t *__restrict __rwlock,
                               clockid_t __clockid,
                               const struct timespec *__restrict __abstime)
{
    return 0;
}

int pthread_rwlock_unlock(pthread_rwlock_t *__rwlock)
{
    return 0;
}
