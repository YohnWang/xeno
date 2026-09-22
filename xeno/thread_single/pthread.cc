#include<pthread.h>
#include<utility>
#include<errno.h>

int pthread_create(pthread_t *thread,
                   const pthread_attr_t *attr,
                   void *(*startroutine)(void *),
                   void *arg)
{
    return EPERM;
}

int pthread_equal(pthread_t t1, pthread_t t2)
{
    return t1 == t2;
}

void pthread_exit(void *value_ptr)
{
    std::unreachable();
}

int pthread_join(pthread_t pthread, void **retval)
{
    return EPERM;
}

int pthread_detach(pthread_t pthread)
{
    return EPERM;
}

pthread_t pthread_self(void)
{
    return 0;
}

int pthread_getschedparam(pthread_t thread,
                          int *policy,
                          struct sched_param *param)
{
    return EPERM;
}

int pthread_setschedparam(pthread_t thread,
                          int policy,
                          const struct sched_param *param)
{
    return EPERM;
}

// libstdc++ need it, see __gthread_active_p
int pthread_cancel(pthread_t __pthread)
{
    return EPERM;
}

int pthread_setname_np(pthread_t thread, const char *name)
{
    return EPERM;
}

int pthread_getname_np(pthread_t thread, char name[], size_t size)
{
    return EPERM;
}

void set_next_thread_stack_size(size_t bytes)
{

}

void set_next_thread_name(const char *name)
{

}

void set_next_thread_priority(int priority)
{

}
