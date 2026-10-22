#include<pthread.h>
#include<string.h>
#include<FreeRTOS.h>
#include<task.h>
#include<semphr.h>
#include<stdio.h>
#include<xeno/kits.h>

using namespace xeno;

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
    if (__kind != PTHREAD_MUTEX_RECURSIVE && __kind != PTHREAD_MUTEX_NORMAL)
        return EINVAL;
    __attr->type = __kind;
    return 0;
}

static void pthread_mutex_try_init(pthread_mutex_t *m)
{
    if (__atomic_load_n(&m->handle , __ATOMIC_ACQUIRE) == nullptr)
    {
        portENTER_CRITICAL();
        if (__atomic_load_n(&m->handle , __ATOMIC_ACQUIRE) == nullptr)
        {
            SemaphoreHandle_t h = nullptr;
            if(m->type == PTHREAD_MUTEX_RECURSIVE)
                h = xSemaphoreCreateRecursiveMutex();
            else
                h = xSemaphoreCreateMutex();
            __atomic_store_n(&m->handle, h, __ATOMIC_RELEASE);
        }
        portEXIT_CRITICAL();
    }
}

static int pthread_mutex_lock_isr(pthread_mutex_t *m)
{
    if(m->handle == nullptr || m->type == PTHREAD_MUTEX_RECURSIVE)
        return EPERM;

    BaseType_t woken = pdFALSE;
    if(xSemaphoreTakeFromISR(m->handle, &woken) != pdTRUE)
        return EBUSY;
    portYIELD_FROM_ISR(woken);
    return 0;
}

static int pthread_mutex_unlock_isr(pthread_mutex_t *m)
{
    if(m->handle == nullptr || m->type == PTHREAD_MUTEX_RECURSIVE || m->owner != nullptr)
        return EPERM;

    BaseType_t woken = pdFALSE;
    if(xSemaphoreGiveFromISR(m->handle, &woken) != pdTRUE)
        return EPERM;
    portYIELD_FROM_ISR(woken);
    return 0;
}

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a)
{
    if(__is_interrupted())
        return EPERM;
    if(a != nullptr)
        m->type = a->type;
    else
        m->type = PTHREAD_MUTEX_NORMAL;
    m->handle = nullptr;
    m->owner = nullptr;
    pthread_mutex_try_init(m);
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *m)
{
    if(__is_interrupted())
        return pthread_mutex_lock_isr(m);
    pthread_mutex_try_init(m);
    BaseType_t r;
    if(m->type == PTHREAD_MUTEX_RECURSIVE)
        r = xSemaphoreTakeRecursive(m->handle, portMAX_DELAY);
    else
        r = xSemaphoreTake(m->handle, portMAX_DELAY);
    if(r != pdTRUE)
        return ETIMEDOUT;
    m->owner = xTaskGetCurrentTaskHandle();
    return 0;
}

// abstime 以 __clockid 为基准：CLOCK_MONOTONIC 由 SysTick 提供，不受
// clock_settime 校时影响；CLOCK_REALTIME 是 MONOTONIC + 一个会被校时改掉的偏移。
// libstdc++ 的 std::timed_mutex/std::recursive_timed_mutex 的
// try_lock_until(steady_clock 期限) 在定义了 _GLIBCXX_USE_PTHREAD_MUTEX_CLOCKLOCK
// 之后走的就是 CLOCK_MONOTONIC 这条路（否则它退化成 system_clock +
// pthread_mutex_timedlock，即下面的 CLOCK_REALTIME 路径）。
int pthread_mutex_clocklock(pthread_mutex_t *__restrict m, clockid_t __clockid,
                            const struct timespec *__restrict ts)
{
    if(__clockid != CLOCK_REALTIME && __clockid != CLOCK_MONOTONIC)
        return EINVAL;

    if(ts != nullptr && (ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000L))
        return EINVAL;

    if(__is_interrupted())
    {
        int e = pthread_mutex_lock_isr(m);
        return e == EBUSY ? ETIMEDOUT : e;
    }

    if(ts == nullptr)
        return pthread_mutex_lock(m);

    pthread_mutex_try_init(m);

    struct timespec start;
    clock_gettime(__clockid, &start);
    struct timespec diff = timespec_diff(&start, ts);
    int64_t ms = diff.tv_sec * 1000LL + diff.tv_nsec / 1000000LL;
    if (ms < 0)
        ms = 0;
    BaseType_t r;
    if(m->type == PTHREAD_MUTEX_RECURSIVE)
        r = xSemaphoreTakeRecursive(m->handle, pdMS_TO_TICKS(ms));
    else
        r = xSemaphoreTake(m->handle, pdMS_TO_TICKS(ms));
    if (r != pdTRUE)
        return ETIMEDOUT;
    m->owner = xTaskGetCurrentTaskHandle();
    return 0;
}

// POSIX: abstime 以 CLOCK_REALTIME 为基准
int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *ts)
{
    return pthread_mutex_clocklock(m, CLOCK_REALTIME, ts);
}

int pthread_mutex_unlock(pthread_mutex_t *m)
{
    if(__is_interrupted())
        return pthread_mutex_unlock_isr(m);
    pthread_mutex_try_init(m);
    if(m->owner != xTaskGetCurrentTaskHandle())
    {
        return EPERM;
    }

    BaseType_t r;
    vTaskSuspendAll();
    if(m->type == PTHREAD_MUTEX_RECURSIVE)
        r = xSemaphoreGiveRecursive(m->handle);
    else
        r = xSemaphoreGive(m->handle);

    if(r == pdTRUE)
    {
        m->owner = (uxSemaphoreGetCount(m->handle) == 0)
                     ? xTaskGetCurrentTaskHandle() : nullptr;
    }
    xTaskResumeAll();

    if (r == pdTRUE)
        return 0;
    return EPERM;
}

int pthread_mutex_trylock(pthread_mutex_t *m)
{
    struct timespec xTimeout = {
        .tv_sec  = 0,
        .tv_nsec = 0
    };
    int e = pthread_mutex_timedlock(m, &xTimeout);
    if(e == ETIMEDOUT)
        e = EBUSY;
    return e;
}

int pthread_mutex_destroy(pthread_mutex_t *m)
{
    if(__is_interrupted())
        return EPERM;
    if(m->handle == nullptr)
        return 0;
    portENTER_CRITICAL();
    vSemaphoreDelete(m->handle);
    m->handle = nullptr;
    portEXIT_CRITICAL();
    return 0;
}
