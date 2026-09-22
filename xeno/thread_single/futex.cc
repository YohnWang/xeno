#include<bits/futex.h>

int __futex_timedwait(int *ptr, int expected, int64_t timeout_millisecond)
{
    return 0;
}

int __futex_wait(int *ptr, int expected)
{
    return 0;
}

void __futex_wake(int *ptr, int value)
{
    return;
}

int __futex_wake_n(int *ptr, int value, int n)
{
    return 0;
}

int __futex_get_wait_numbers(int *ptr)
{
    return 0;
}

size_t __futex_get_pool_size()
{
    return 0;
}

size_t __futex_get_pool_capacity()
{
    return 0;
}