#include <semaphore.h>
#include <time.h>
#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>
#include <errno.h>

#include <xeno/kits.h>

using namespace xeno;

int sem_init(sem_t *__sem, int __pshared, unsigned int __value)
{
    __sem->handle = xSemaphoreCreateCounting(SEM_VALUE_MAX, __value);
    return 0;
}

int sem_destroy(sem_t *__sem)
{
    vSemaphoreDelete(__sem->handle);
    __sem->handle = 0;
    return 0;
}

int sem_wait(sem_t *__sem)
{
    xSemaphoreTake(__sem->handle, portMAX_DELAY);
    return 0;
}

int sem_timedwait(sem_t *__restrict __sem, const struct timespec *__restrict __abstime)
{
    if(__abstime->tv_nsec < 0 || __abstime->tv_nsec >= 1'000'000'000)
    {
        errno = EINVAL;
        return -1;
    }
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
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

/* Test whether SEM is posted.  */
int sem_trywait(sem_t *__sem)
{
    BaseType_t r = xSemaphoreTake(__sem->handle, 0);
    if(r == pdTRUE)
        return 0;
    errno = EAGAIN;
    return -1;
}

/* Post SEM.  */
int sem_post(sem_t *__sem)
{
    BaseType_t r = xSemaphoreGive(__sem->handle);
    if(r == pdTRUE)
        return 0;
    errno = EOVERFLOW;
    return -1;
}

int sem_getvalue(sem_t *__restrict __sem, int *__restrict __sval)
{
    UBaseType_t v = uxSemaphoreGetCount(__sem->handle);
    *__sval = v;
    return 0;
}
