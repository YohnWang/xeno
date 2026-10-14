// 读写锁。状态的维护方式跟互斥量不同：不用 FreeRTOS 句柄，全部放在锁对象里
// （见 _pthreadtypes.h），等待则复用 xeno 的 futex（bits/futex.h），和条件变量一样。
//
//   state           0 = 空闲；> 0 = 读者数；-1 = 写者持有
//   pending_writers 正在等待的写者数；> 0 时新的读者不再进入，
//                   否则源源不断的读者会把写者饿死（写者优先策略）
//   event           futex 事件字：状态每次变化就自增并唤醒所有等待者；
//                   等待者睡在它上面，醒来后重新读 state，所以唤醒是广播式的
//
// event 必须用 fetch_add 取新值，不能由唤醒者写"自己读到的值 + 1"：两次并发的
// 状态变化可能算出一模一样的新值，而等待者判断"要不要继续睡"比的就是这个值，
// 于是已经排好队的等待者会错过唤醒（锁明明空出来了却一直睡到超时）。
//
// 已知取舍：
//   - 写者优先：线程已经持读锁、再取一次读锁时，如果恰好有写者在等待，
//     它会等写者先过完（和 glibc 的 PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP
//     一致）。没有写者等待时，同一线程重复取读锁是允许的。
//   - 等待走 futex 里的信号量，不参与 FreeRTOS 的优先级继承（条件变量同理）。

#include<pthread.h>
#include<time.h>
#include<errno.h>
#include<stdint.h>
#include<FreeRTOS.h>
#include<task.h>

#include<bits/futex.h>
#include<xeno/kits.h>

using namespace xeno;

namespace
{

constexpr int writer_held = -1;
constexpr int64_t wait_forever = -1;

// abstime 是绝对时间，换算成还剩多少毫秒（已经过期就是 0；
// 不能返回负数，__futex_timedwait 拿到负数会当作"一直等"）
int64_t remaining_milliseconds(clockid_t clockid, const struct timespec *abstime)
{
    struct timespec start;
    clock_gettime(clockid, &start);
    struct timespec diff = timespec_diff(&start, abstime);
    int64_t ms = diff.tv_sec * 1000LL + diff.tv_nsec / 1000000LL;
    return ms < 0 ? 0 : ms;
}

// 唤醒所有等待者（唤醒后它们会重新检查 state，所以多叫醒几个没关系）
void wake_waiters(pthread_rwlock_t *rw)
{
    int event = __atomic_fetch_add(&rw->event, 1, __ATOMIC_ACQ_REL);
    __futex_wake(&rw->event, event + 1);
}

// 现在能不能立刻拿到：0 = 拿到，EBUSY = 拿不到
int try_rdlock(pthread_rwlock_t *rw)
{
    for(;;)
    {
        int state = __atomic_load_n(&rw->state, __ATOMIC_ACQUIRE);

        // 写者持有，或者有写者在排队（写者优先），读者都不进
        if(state < 0 || __atomic_load_n(&rw->pending_writers, __ATOMIC_ACQUIRE) > 0)
            return EBUSY;

        if(__atomic_compare_exchange_n(&rw->state, &state, state + 1,
                                       false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return 0;
    }
}

int try_wrlock(pthread_rwlock_t *rw)
{
    int state = 0;
    if(__atomic_compare_exchange_n(&rw->state, &state, writer_held,
                                   false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
        // 记下持有者：解锁时要检查，trywrlock 成功这条路也得记
        rw->writer = xTaskGetCurrentTaskHandle();
        return 0;
    }
    return EBUSY;
}

// 等到 abstime（以 clockid 计时）为止；abstime == nullptr 表示一直等，
// 这时 clockid 不参与计算。
// trylock 返回 0 就结束，否则睡在 event 上，被唤醒后重新试。
template<class TryLock>
int wait_for_lock(pthread_rwlock_t *rw, clockid_t clockid,
                  const struct timespec *abstime, TryLock trylock)
{
    int64_t remain = abstime == nullptr
                       ? wait_forever : remaining_milliseconds(clockid, abstime);

    for(;;)
    {
        if(trylock() == 0)
            return 0;

        // 先读 event 再睡：__futex_timedwait 在它已经变了的时候会立刻返回，
        // 所以在这两步之间发生的状态变化不会丢掉
        int event = __atomic_load_n(&rw->event, __ATOMIC_ACQUIRE);
        if(__futex_timedwait(&rw->event, event,
                             remain < 0 ? INT64_MAX : remain) == ETIMEDOUT)
        {
            // 超时的那一瞬间可能正好被放行，再试一次再报超时
            return trylock() == 0 ? 0 : ETIMEDOUT;
        }

        if(abstime != nullptr)
            remain = remaining_milliseconds(clockid, abstime);
    }
}

int acquire_write(pthread_rwlock_t *rw, clockid_t clockid,
                  const struct timespec *abstime)
{
    // 先声明"有写者在等"，让新读者别进来，否则读者不断进来写者永远等不到
    __atomic_fetch_add(&rw->pending_writers, 1, __ATOMIC_ACQ_REL);

    int r = wait_for_lock(rw, clockid, abstime, [rw]{ return try_wrlock(rw); });

    __atomic_fetch_sub(&rw->pending_writers, 1, __ATOMIC_ACQ_REL);
    if(r != 0)
        wake_waiters(rw);   // 之前被上面那个计数挡住的读者可以进来了

    return r;
}

int check_clock_and_timeout(clockid_t clockid, const struct timespec *abstime)
{
    if(clockid != CLOCK_REALTIME && clockid != CLOCK_MONOTONIC)
        return EINVAL;

    if(abstime != nullptr && (abstime->tv_nsec < 0 || abstime->tv_nsec >= 1000000000L))
        return EINVAL;

    return 0;
}

}

int pthread_rwlock_init(pthread_rwlock_t *__rwlock, const pthread_rwlockattr_t *__attr)
{
    *__rwlock = {};
    return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *__rwlock)
{
    if(__rwlock->state != 0 || __futex_get_wait_numbers(&__rwlock->event) > 0)
        return EBUSY;

    return 0;
}

int pthread_rwlockattr_init(pthread_rwlockattr_t *__attr)
{
    __attr->is_initialized = 1;
    return 0;
}

int pthread_rwlockattr_destroy(pthread_rwlockattr_t *__attr)
{
    __attr->is_initialized = 0;
    return 0;
}

int pthread_rwlockattr_getpshared(const pthread_rwlockattr_t *__attr, int *__pshared)
{
    return -1;
}

int pthread_rwlockattr_setpshared(pthread_rwlockattr_t *__attr, int __pshared)
{
    return -1;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *__rwlock)
{
    return wait_for_lock(__rwlock, CLOCK_MONOTONIC, nullptr,
                         [__rwlock]{ return try_rdlock(__rwlock); });
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *__rwlock)
{
    return try_rdlock(__rwlock);
}

// POSIX: pthread_rwlock_timedrdlock 的 abstime 以 CLOCK_REALTIME 为基准
int pthread_rwlock_timedrdlock(pthread_rwlock_t *__restrict __rwlock,
                               const struct timespec *__restrict __abstime)
{
    return pthread_rwlock_clockrdlock(__rwlock, CLOCK_REALTIME, __abstime);
}

int pthread_rwlock_clockrdlock(pthread_rwlock_t *__restrict __rwlock,
                               clockid_t __clockid,
                               const struct timespec *__restrict __abstime)
{
    if(int e = check_clock_and_timeout(__clockid, __abstime); e != 0)
        return e;

    return wait_for_lock(__rwlock, __clockid, __abstime,
                         [__rwlock]{ return try_rdlock(__rwlock); });
}

int pthread_rwlock_wrlock(pthread_rwlock_t *__rwlock)
{
    return acquire_write(__rwlock, CLOCK_MONOTONIC, nullptr);
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *__rwlock)
{
    return try_wrlock(__rwlock);
}

// POSIX: pthread_rwlock_timedwrlock 的 abstime 以 CLOCK_REALTIME 为基准
int pthread_rwlock_timedwrlock(pthread_rwlock_t *__restrict __rwlock,
                               const struct timespec *__restrict __abstime)
{
    return pthread_rwlock_clockwrlock(__rwlock, CLOCK_REALTIME, __abstime);
}

int pthread_rwlock_clockwrlock(pthread_rwlock_t *__restrict __rwlock,
                               clockid_t __clockid,
                               const struct timespec *__restrict __abstime)
{
    if(int e = check_clock_and_timeout(__clockid, __abstime); e != 0)
        return e;

    return acquire_write(__rwlock, __clockid, __abstime);
}

int pthread_rwlock_unlock(pthread_rwlock_t *__rwlock)
{
    for(;;)
    {
        int state = __atomic_load_n(&__rwlock->state, __ATOMIC_ACQUIRE);

        if(state > 0)
        {
            // 读者解锁：只有最后一个读者离开时才需要叫醒可能在等的写者
            if(__atomic_compare_exchange_n(&__rwlock->state, &state, state - 1,
                                           false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            {
                if(state == 1)
                    wake_waiters(__rwlock);
                return 0;
            }
            continue;
        }

        if(state == writer_held)
        {
            // POSIX 规定必须由持有者解锁，这里和互斥量一样查一下
            if(__rwlock->writer != xTaskGetCurrentTaskHandle())
                return EPERM;

            // owner 必须赶在放开 state 之前清掉：state 一旦变成 0，别的任务
            // 可能马上就拿到写锁并写下自己的句柄，那时再清就把它的抹掉了
            __rwlock->writer = nullptr;
            if(__atomic_compare_exchange_n(&__rwlock->state, &state, 0,
                                           false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            {
                wake_waiters(__rwlock);
                return 0;
            }

            __rwlock->writer = xTaskGetCurrentTaskHandle();   // CAS 失败，把 owner 写回去
            continue;
        }

        return EPERM;   // 没加锁就解锁
    }
}
