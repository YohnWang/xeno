#include<pthread.h>
#include<FreeRTOS.h>
#include<task.h>

#include<array>
#include<mutex>
#include<ranges>

#include<xeno/kits.h>

namespace xeno
{

struct key_t
{
    bool is_used = false;
    void (*destructor)(void *) = nullptr;
};

static std::mutex m;
static constinit std::array<key_t, configNUM_THREAD_LOCAL_STORAGE_POINTERS> pthread_keys = {{true}};

}

using namespace xeno;

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *))
{
    if(key == nullptr)
        return EINVAL;

    std::lock_guard _{m};
    int counter = 0;
    for(auto &i: pthread_keys)
    {
        if(i.is_used == false)
        {
            *key = counter;
            i.is_used = true;
            i.destructor = destructor;
            return 0;
        }
        counter++;
    }
    return ENOMEM;
}

int pthread_setspecific(pthread_key_t key, const void *value)
{
    if(key >= std::size(pthread_keys))
        return EINVAL;
    vTaskSetThreadLocalStoragePointer(nullptr, key, const_cast<void*>(value));
    return 0;
}

void *pthread_getspecific(pthread_key_t key)
{
    if(key >= std::size(pthread_keys))
        return nullptr;
    return pvTaskGetThreadLocalStoragePointer(nullptr, key);
}

int pthread_key_delete(pthread_key_t key)
{
    if(key >= std::size(pthread_keys))
        return EINVAL;
    std::lock_guard _{m};
    pthread_keys[key].is_used = false;
    pthread_keys[key].destructor = nullptr;
    return 0;
}

// 得写 xeno:: 限定：全局命名空间里还有一个 POSIX 的 IPC key_t
static std::array<xeno::key_t, configNUM_THREAD_LOCAL_STORAGE_POINTERS> keys_snapshot()
{
    std::lock_guard _{m};
    return pthread_keys;
}

// 线程退出时的清理：调用所有 pthread key 的析构函数。
// std::promise::set_value_at_thread_exit、std::notify_all_at_thread_exit 和
// __cxa_thread_atexit（thread_local 对象的析构）都挂在这个机制上。
extern "C"
void __pthread_key_destruct(TaskHandle_t handler)
{
    // 每轮重新取一份 key 表（顺带把锁放掉）：析构函数里可能再调
    // pthread_key_create/delete，持着锁调用户代码会死锁。析构函数刚注册的
    // key 要先回到下一轮的快照里才看得到。
    for(int iteration = 0; iteration < PTHREAD_DESTRUCTOR_ITERATIONS; iteration++)
    {
        auto keys = keys_snapshot();
        bool again = false;

        for(size_t i = 0; i < keys.size(); i++)
        {
            if(!keys[i].is_used || keys[i].destructor == nullptr)
                continue;

            auto ptr = pvTaskGetThreadLocalStoragePointer(handler, i);
            if(ptr == nullptr)
                continue;

            // POSIX：调析构函数之前先把槽置空。这也让本函数天然幂等——
            // pthread_exit() 里跑过一次之后，vTaskDelete() 的 traceENTER
            // 钩子还会用同一条线程再调一次。
            vTaskSetThreadLocalStoragePointer(handler, i, nullptr);
            keys[i].destructor(ptr);
            again = true;
        }

        if(!again)
            return;     // 已经没有要析构的值了
    }

    // 迭代次数用完：POSIX 规定不再调析构函数，剩下的值直接丢弃。
    // 这里必须真的清空——最后一轮里析构函数可能又给自己赋了值，而
    // vTaskDelete 钩子那次调用会让它多跑一轮。
    // 只清带析构函数的槽位：槽 0 放的是线程自己的 pthread_internal_t，
    // 它 is_used 为 true 但析构函数是空的，得留着。
    auto keys = keys_snapshot();
    for(size_t i = 0; i < keys.size(); i++)
    {
        if(keys[i].is_used && keys[i].destructor != nullptr)
            vTaskSetThreadLocalStoragePointer(handler, i, nullptr);
    }
}