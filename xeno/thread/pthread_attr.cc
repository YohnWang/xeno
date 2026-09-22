#include<pthread.h>
#include<errno.h>

#include<FreeRTOS.h>

static const pthread_attr_t pthread_attr_default = {
    .stackaddr = nullptr,
    .stacksize = 8 * 1024,
    .param = {.sched_priority = 0},
    .detachstate = PTHREAD_CREATE_JOINABLE
};

int pthread_attr_init(pthread_attr_t *attr)
{
    *attr = pthread_attr_default;
    return 0;
}

int pthread_attr_destroy(pthread_attr_t *attr)
{
    (void)attr;
    return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *detachstate)
{
    *detachstate = attr->detachstate;
    return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate)
{
    if(detachstate != PTHREAD_CREATE_JOINABLE && detachstate != PTHREAD_CREATE_DETACHED)
        return EINVAL;
    attr->detachstate = detachstate;
    return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize)
{
    if(stacksize < configMINIMAL_STACK_SIZE)
        return EINVAL;
    attr->stacksize = stacksize;
    return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t * attr, size_t * stacksize)
{
    *stacksize = attr->stacksize;
    return 0;
}

int pthread_attr_setschedpolicy(pthread_attr_t *attr, int policy)
{
    return 0;
}

int pthread_attr_getschedpolicy(const pthread_attr_t * attr, int * policy)
{
    return 0;
}

int pthread_attr_setschedparam(pthread_attr_t *attr, const struct sched_param *param)
{
    attr->param = *param;
    return 0;
}

int pthread_attr_getschedparam(const pthread_attr_t *attr, struct sched_param *param)
{
    *param = attr->param;
    return 0;
}