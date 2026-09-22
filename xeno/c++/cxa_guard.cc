#include <bits/futex.h>

static inline int __guard_test_bit(const int __byte, const int __val)
{
    union
    {
        int __i;
        char __c[sizeof(int)];
    } __u = {0};
    __u.__c[__byte] = __val;
    return __u.__i;
}

#define _GLIBCXX_GUARD_BIT __guard_test_bit(0, 1)
#define _GLIBCXX_GUARD_PENDING_BIT __guard_test_bit(1, 1)
#define _GLIBCXX_GUARD_WAITING_BIT __guard_test_bit(2, 1)

extern "C"{

int __wrap___cxa_guard_acquire(long *g)
{
    while (true)
    {
        int expected = 0;
        int *gi = (int *)(void *)g;
        const int guard_bit = _GLIBCXX_GUARD_BIT;
        const int pending_bit = _GLIBCXX_GUARD_PENDING_BIT;
        const int waiting_bit = _GLIBCXX_GUARD_WAITING_BIT;
        // 如果*g == 0, 则未初始化，需要初始化
        if (__atomic_compare_exchange_n(gi, &expected, pending_bit, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        {
            return 1;
        }

        if (expected == guard_bit)
        {
            // Already initialized.
            return 0;
        }

        if (expected == pending_bit)
        {
            // Use acquire here.
            int newv = expected | waiting_bit;
            if (!__atomic_compare_exchange_n(gi, &expected, newv, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            {
                if (expected == guard_bit)
                {
                    // Make a thread that failed to set the
                    // waiting bit exit the function earlier,
                    // if it detects that another thread has
                    // successfully finished initialising.
                    return 0;
                }
                if (expected == 0)
                    continue;
            }

            expected = newv;
        }

        __futex_wait(gi, expected);
    }
}

void __wrap___cxa_guard_release(long *g)
{
    int *gi = (int *)(void *)g;
    const int guard_bit = _GLIBCXX_GUARD_BIT;
    const int waiting_bit = _GLIBCXX_GUARD_WAITING_BIT;
    int old = __atomic_exchange_n(gi, guard_bit, __ATOMIC_ACQ_REL);

    if ((old & waiting_bit) != 0)
        __futex_wake(gi, guard_bit);
}

void __wrap___cxa_guard_abort(long *g)
{
    int *gi = (int *)(void *)g;
    __atomic_store_n(gi, 0, __ATOMIC_RELEASE);
}

}