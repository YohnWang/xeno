#include <pthread.h>

int pthread_once(pthread_once_t *__once_control, void (*__init_routine)(void))
{
    if(__once_control->init_executed == 0)
    {
        (*__init_routine)();
        __once_control->init_executed = 1;
    }

    return 0;
}