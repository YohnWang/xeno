#include<FreeRTOS.h>
#include<task.h>
#include<stdio.h>
#include<string.h>
#include<pthread.h>
#include<errno.h>
#include<inplace_vector>
#include<algorithm>
#include<numeric>
#include<bits/futex.h>
#include<xeno/kits.h>
#include"common.h"

// 用平凡的聚合而不是 std::pair：libstdc++ 的 inplace_vector 在常量初始化时
// 对非平凡元素走 __builtin_unreachable()，constinit 编译不过；去掉 constinit
// 又会生成 .init_array 里的启动构造函数。
struct wait_entry
{
    int *ptr;
    SemaphoreHandle_t semaphore;
};

static constexpr size_t futex_pool_capacity = 128;
static constinit std::inplace_vector<wait_entry, futex_pool_capacity> waiting_semaphores;

static void insert_waiting_semaphores(wait_entry entry)
{
    auto slot = waiting_semaphores.try_push_back(entry);
    (void)slot;
    configASSERT(slot.has_value());
}

static void erase_waiting_semaphores(wait_entry entry)
{
    auto &v = waiting_semaphores;
    auto iter = std::find_if(v.begin(), v.end(), [&entry](const wait_entry &i)
    {
        return i.ptr == entry.ptr && i.semaphore == entry.semaphore;
    });
    if(iter != v.end())
    {
        v.erase(iter);
    }
}

int __futex_timedwait(int *ptr, int expected, int64_t timeout_millisecond)
{
    int value = __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
    if(value != expected)
        return 0;

    TickType_t last_tick = xTaskGetTickCount();
    int64_t timeout_surplus = timeout_millisecond;
    SemaphoreHandle_t h = nullptr;
    StaticSemaphore_t s = {};
    bool once = true;

    while (value == expected && timeout_surplus > 0)
    {
        if(once)
        {
            xeno::critical_lock guard;
            int value = __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
            if(value != expected)
                return 0;
            h = xSemaphoreCreateBinaryStatic(&s);
            insert_waiting_semaphores({ptr, h});
            once = false;
        }

        constexpr int64_t max_timeout_ms = std::numeric_limits<int>::max();

        xSemaphoreTake(h, pdMS_TO_TICKS(std::min(timeout_surplus, max_timeout_ms)));

        auto current = xTaskGetTickCount();
        timeout_surplus = timeout_surplus - pdTICKS_TO_MS(current - last_tick);
        last_tick = current;
        value = __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
    }

    if(h)
    {
        {
            xeno::critical_lock guard;
            erase_waiting_semaphores({ptr, h});
        }
        // 摘除之后中断再也找不到这个句柄，删除是安全的
        vSemaphoreDelete(h);
    }

    if(value != expected)
        return 0;
    return ETIMEDOUT;
}

int __futex_wait(int *ptr, int expected)
{
    while(__futex_timedwait(ptr, expected, std::numeric_limits<int64_t>::max()) == ETIMEDOUT){}
    return 0;
}

void __futex_wake(int *ptr, int value)
{
    const bool isr = __is_interrupted();
    BaseType_t woken = pdFALSE;

    {
        xeno::critical_lock guard;
        __atomic_store_n(ptr, value, __ATOMIC_RELEASE);

        for(const auto &i: waiting_semaphores)
        {
            if(i.ptr != ptr)
                continue;

            if(isr)
            {
                BaseType_t w = pdFALSE;
                (void)xSemaphoreGiveFromISR(i.semaphore, &w);
                if(w != pdFALSE)
                    woken = pdTRUE;
            }
            else
            {
                (void)xSemaphoreGive(i.semaphore);
            }
        }
    }

    // 必须等中断屏蔽解除之后再请求切换
    if(isr)
        portYIELD_FROM_ISR(woken);
}

int __futex_wake_n(int *ptr, int value, int n)
{
    const bool isr = __is_interrupted();
    BaseType_t woken = pdFALSE;
    int counter = 0;

    {
        xeno::critical_lock guard;
        __atomic_store_n(ptr, value, __ATOMIC_RELEASE);

        for(const auto &i: waiting_semaphores)
        {
            if(counter >= n)
                break;

            if(i.ptr != ptr)
                continue;

            // 只有真的给出去了才算一次唤醒：等待者被唤醒后还要跑起来才会把自己从
            // waiting_semaphores 里摘掉，在那之前它那个（二进制）信号量仍然是满的，
            // give 会返回 errQUEUE_FULL（FromISR 版本一样）。要是把失败的这次也计数
            // 并 break，连续两次 signal 就只会喂到同一个等待者，第二个永远收不到通知。
            BaseType_t r;
            if(isr)
            {
                BaseType_t w = pdFALSE;
                r = xSemaphoreGiveFromISR(i.semaphore, &w);
                if(w != pdFALSE)
                    woken = pdTRUE;
            }
            else
            {
                r = xSemaphoreGive(i.semaphore);
            }

            if(r == pdTRUE)
                counter++;
        }
    }

    // 必须等中断屏蔽解除之后再请求切换
    if(isr)
        portYIELD_FROM_ISR(woken);

    return counter;
}

int __futex_get_wait_numbers(int *ptr)
{
    int size = 0;
    xeno::critical_lock guard;
    for (const auto &i: waiting_semaphores)
    {
        if(i.ptr == ptr)
            size++;
    }
    return size;
}

size_t __futex_get_pool_size()
{
    xeno::critical_lock guard;
    return waiting_semaphores.size();
}

size_t __futex_get_pool_capacity()
{
    xeno::critical_lock guard;
    return waiting_semaphores.capacity();
}
