#include <semaphore.h>
#include <time.h>
#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>
#include <errno.h>

#include <xeno/kits.h>

using namespace xeno;

static bool sem_take_isr(SemaphoreHandle_t h)
{
    BaseType_t woken = pdFALSE;
    if(xSemaphoreTakeFromISR(h, &woken) != pdTRUE)
        return false;
    portYIELD_FROM_ISR(woken);
    return true;
}

int sem_init(sem_t *__sem, int __pshared, unsigned int __value)
{
    if(__is_interrupted())
    {
        errno = EPERM;
        return -1;
    }

    if(__value > (unsigned int)SEM_VALUE_MAX)
    {
        errno = EINVAL;
        return -1;
    }

    __sem->handle = xSemaphoreCreateCounting(SEM_VALUE_MAX, __value);
    if(__sem->handle == nullptr)
    {
        errno = ENOSPC;
        return -1;
    }
    return 0;
}

int sem_destroy(sem_t *__sem)
{
    if(__is_interrupted())
    {
        errno = EPERM;
        return -1;
    }
    vSemaphoreDelete(__sem->handle);
    __sem->handle = 0;
    return 0;
}

int sem_wait(sem_t *__sem)
{
    if(__is_interrupted())
    {
        if(sem_take_isr(__sem->handle))
            return 0;
        errno = EAGAIN;
        return -1;
    }
    xSemaphoreTake(__sem->handle, portMAX_DELAY);
    return 0;
}

int sem_clockwait(sem_t *__restrict __sem, clockid_t __clockid,
                  const struct timespec *__restrict __abstime)
{
    if(__clockid != CLOCK_REALTIME && __clockid != CLOCK_MONOTONIC)
    {
        errno = EINVAL;
        return -1;
    }

    if(__abstime->tv_nsec < 0 || __abstime->tv_nsec >= 1'000'000'000)
    {
        errno = EINVAL;
        return -1;
    }

    if(__is_interrupted())
    {
        if(sem_take_isr(__sem->handle))
            return 0;
        errno = ETIMEDOUT;
        return -1;
    }

    struct timespec start;
    clock_gettime(__clockid, &start);
    struct timespec diff = timespec_diff(&start, __abstime);
    int64_t ms = diff.tv_sec * 1000LL + diff.tv_nsec / 1000000LL;
    if (ms < 0)
        ms = 0;
    BaseType_t r = xSemaphoreTake(__sem->handle, pdMS_TO_TICKS(ms));
    if (r == pdTRUE)
        return 0;
    errno = ETIMEDOUT;
    return -1;
}

int sem_timedwait(sem_t *__restrict __sem, const struct timespec *__restrict __abstime)
{
    return sem_clockwait(__sem, CLOCK_REALTIME, __abstime);
}

/* Test whether SEM is posted.  */
int sem_trywait(sem_t *__sem)
{
    if(__is_interrupted())
    {
        if(sem_take_isr(__sem->handle))
            return 0;
        errno = EAGAIN;
        return -1;
    }

    BaseType_t r = xSemaphoreTake(__sem->handle, 0);
    if(r == pdTRUE)
        return 0;
    errno = EAGAIN;
    return -1;
}

/* Post SEM.  */
int sem_post(sem_t *__sem)
{
    if(__is_interrupted())
    {
        BaseType_t woken = pdFALSE;
        if(xSemaphoreGiveFromISR(__sem->handle, &woken) != pdTRUE)
        {
            errno = EOVERFLOW;
            return -1;
        }
        portYIELD_FROM_ISR(woken);
        return 0;
    }

    BaseType_t r = xSemaphoreGive(__sem->handle);
    if(r == pdTRUE)
        return 0;
    errno = EOVERFLOW;
    return -1;
}

int sem_getvalue(sem_t *__restrict __sem, int *__restrict __sval)
{
    UBaseType_t v = __is_interrupted()
                  ? uxSemaphoreGetCountFromISR(__sem->handle)
                  : uxSemaphoreGetCount(__sem->handle);
    *__sval = v;
    return 0;
}
