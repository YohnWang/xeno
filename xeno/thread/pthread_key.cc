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

extern "C"
void __pthread_key_destruct(TaskHandle_t handler)
{
    std::lock_guard _{m};
    for(size_t i = 0; i < pthread_keys.size(); i++)
    {
        if(pthread_keys[i].is_used && pthread_keys[i].destructor)
        {
            auto ptr = pvTaskGetThreadLocalStoragePointer(handler, i);
            if(ptr)
            {
                pthread_keys[i].destructor(ptr);
            }
        }
    }
}