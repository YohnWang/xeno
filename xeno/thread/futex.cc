#include<FreeRTOS.h>
#include<task.h>
#include<stdio.h>
#include<string.h>
#include<pthread.h>
#include<errno.h>
#include<vector>
#include<algorithm>
#include<numeric>
#include<mutex>
#include<bits/futex.h>
#include<xeno/kits.h>
#include"common.h"

static xeno::scheduler_lock lock;
static constinit std::vector<std::pair<int*, SemaphoreHandle_t>> waiting_semaphores;

static void insert_waiting_semaphores(std::pair<int*, SemaphoreHandle_t> pair)
{
    auto &v = waiting_semaphores;
    if(std::find(v.begin(), v.end(), pair) == v.end())
    {
        v.push_back(pair);
    }
}

static void erase_waiting_semaphores(std::pair<int*, SemaphoreHandle_t> pair)
{
    auto &v = waiting_semaphores;
    if(auto iter = std::find(v.begin(), v.end(), pair); iter != v.end())
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
            std::lock_guard _{lock};
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
            std::lock_guard _{lock};
            erase_waiting_semaphores({ptr, h});
        }
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
    std::lock_guard _{lock};
    __atomic_store_n(ptr, value, __ATOMIC_RELEASE);

    for(auto i: waiting_semaphores)
    {
        if(i.first == ptr)
            xSemaphoreGive(i.second);
    }
}

int __futex_wake_n(int *ptr, int value, int n)
{
    std::lock_guard _{lock};
    __atomic_store_n(ptr, value, __ATOMIC_RELEASE);
    int counter = 0;
    for(auto i: waiting_semaphores)
    {
        if(counter < n)
        {
            if(i.first == ptr)
            {
                xSemaphoreGive(i.second);
                counter++;
            }
        }
        else
        {
            break;
        }
    }

    return counter;
}

int __futex_get_wait_numbers(int *ptr)
{
    int size = 0;
    std::lock_guard _{lock};
    for (auto &i: waiting_semaphores)
    {
        if(i.first == ptr)
            size++;
    }
    return size;
}

size_t __futex_get_pool_size()
{
    std::lock_guard _{lock};
    return waiting_semaphores.size();
}

size_t __futex_get_pool_capacity()
{
    std::lock_guard _{lock};
    return waiting_semaphores.capacity();
}
