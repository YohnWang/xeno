#include<pthread.h>
#include<bits/futex.h>
#include<xeno/kits.h>

// 0 未初始化
// 1 初始化中
// 2 初始化完成
extern "C"
int pthread_once(pthread_once_t *__once_control, void (*__init_routine)(void))
{
    int expected = 0;
    if (__atomic_compare_exchange_n(&__once_control->init_executed, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
        (*__init_routine)();
        __futex_wake(&__once_control->init_executed, 2);
        return 0;
    }
    __futex_wait(&__once_control->init_executed, 1);
    return 0;
}