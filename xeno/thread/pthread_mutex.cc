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
    if (__kind != PTHREAD_MUTEX_RECURSIVE || __kind != PTHREAD_MUTEX_NORMAL)
        return -1;
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

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a)
{
    if(a != nullptr)
        m->type = a->type;
    else
        m->type = PTHREAD_MUTEX_NORMAL;
    m->handle = nullptr;
    pthread_mutex_try_init(m);
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *m)
{
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

int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *ts)
{
    if(ts == nullptr)
        return pthread_mutex_lock(m);

    pthread_mutex_try_init(m);

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
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

int pthread_mutex_unlock(pthread_mutex_t *m)
{
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
    m->owner = xTaskGetCurrentTaskHandle();
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
    if(m->handle == nullptr)
        return 0;
    portENTER_CRITICAL();
    vSemaphoreDelete(m->handle);
    m->handle = nullptr;
    portEXIT_CRITICAL();
    return 0;
}
