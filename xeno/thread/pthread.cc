#include<pthread.h>
#include<errno.h>
#include<string.h>
#include<FreeRTOS.h>
#include<task.h>

#include<vector>
#include<future>
#include<experimental/scope>

#include<bits/futex.h>
#include<xeno/kits.h>
#include<stdio.h>

static thread_local int next_thread_priority = 0;
static thread_local void *next_thread_stack_ptr = nullptr;
static thread_local size_t next_thread_stack_size = 0;
static thread_local const char *next_thread_name = nullptr;

extern "C"
{

void set_next_thread_stack(void *ptr, size_t bytes)
{
    next_thread_stack_ptr = ptr;
    next_thread_stack_size = bytes;
}

void set_next_thread_stack_size(size_t bytes)
{
    next_thread_stack_ptr = nullptr;
    next_thread_stack_size = bytes;
}

void set_next_thread_name(const char *name)
{
    next_thread_name = name;
}

void set_next_thread_priority(int priority)
{
    next_thread_priority = priority;
}

struct pthread_internal_t
{
    void* (*routine)(void*) = nullptr;
    void *args = nullptr;
    int detachstate = PTHREAD_CREATE_DETACHED;
    void *result = nullptr;
    int joinstate = 0;
    pthread_t join_thread = nullptr;
};

static pthread_internal_t *pthread_internal(pthread_t th = nullptr)
{
    auto internal = (pthread_internal_t *)pvTaskGetThreadLocalStoragePointer((TaskHandle_t)th, 0);
    if(internal == nullptr)
    {
        // freerots启动的线程，共享同一个只读pthread信息
        static const pthread_internal_t origin = {};
        internal = const_cast<pthread_internal_t*>(&origin);
        vTaskSetThreadLocalStoragePointer((TaskHandle_t)th, 0, internal);
    }
    return internal;
}

static bool is_pthread_create(pthread_t th)
{
    auto internal = pthread_internal(th);
    return internal->routine != nullptr;
}

static void freertos_task(void *args)
{
    pthread_internal_t *internal = (pthread_internal_t *)args;
    void *result = internal->routine(internal->args);
    pthread_exit(result);
}

int pthread_create(pthread_t *thread,
                   const pthread_attr_t *attr_,
                   void *(*startroutine)(void *),
                   void *arg)
{
    // 调用此函数后，无论是否成功，重置外部设置
    std::experimental::scope_exit _ { []{
        next_thread_stack_ptr = nullptr;
        next_thread_stack_size = 0;
        next_thread_name = nullptr;
        next_thread_priority = 0;
    }};

    char buf[32] = "unknown";
    const char *name = buf;

    if(next_thread_name != nullptr)
    {
        name = next_thread_name;
    }
    else
    {
        pthread_getname_np(pthread_self(), buf, sizeof buf);
        size_t n = strlen(buf);
        if(n < sizeof(buf) - 1)
        {
            buf[n] = '@';
            buf[n + 1] = '\0';
        }
    }

    pthread_attr_t attr;
    if(attr_ == nullptr)
    {
        pthread_attr_init(&attr);
        if(next_thread_stack_size != 0)
            attr.stacksize = next_thread_stack_size;
        if(next_thread_priority != 0)
            attr.param.sched_priority = next_thread_priority;
    }
    else
    {
        attr = *attr_;
    }

    pthread_internal_t *internal = new(std::nothrow) pthread_internal_t;
    if(internal == nullptr)
        return EAGAIN;
    
    vTaskSuspendAll();
    std::experimental::scope_exit _ {[&internal]{
        if(internal != nullptr)
            delete (internal);
        xTaskResumeAll();
    }};

    internal->args = arg;
    internal->routine = startroutine;
    internal->detachstate = attr.detachstate;

    TaskHandle_t handler;

    // 未指定栈时，动态分配
    if(attr.stackaddr == nullptr)
    {
        auto ret = xTaskCreate(freertos_task, name, (attr.stacksize + 3)/sizeof(StackType_t),
                                internal, attr.param.sched_priority, &handler);
        if(ret != pdPASS)
        {
            return EAGAIN;
        }
            
    }
    else
    {
        if((size_t)attr.stacksize < configMINIMAL_STACK_SIZE + sizeof(StaticTask_t))
            return EINVAL;
        
        // TODO 对齐处理
        StaticTask_t *tcb = (StaticTask_t *)attr.stackaddr;
        StackType_t *stack = reinterpret_cast<StackType_t *>(tcb + 1);
        size_t size = attr.stacksize - sizeof(StaticTask_t);
        handler = xTaskCreateStatic(freertos_task, name, (size)/sizeof(StackType_t),
                                            internal, attr.param.sched_priority, stack, tcb);
    }

    vTaskSetThreadLocalStoragePointer(handler, 0, internal);

    if(thread)
        *thread = (pthread_t)handler;
    internal = nullptr; // don't free
    
    return 0;
}

void pthread_exit(void *value_ptr)
{
    auto internal = pthread_internal();

    if(internal->routine == nullptr)
    {
        vTaskDelete(nullptr);
        __builtin_unreachable();
    }

    internal->result = value_ptr;

    if(internal->detachstate == PTHREAD_CREATE_JOINABLE)
    {
        __futex_wake(&internal->joinstate, 1);
        vTaskSuspend(nullptr);
    }
    else
    {
        delete internal;
        vTaskDelete(nullptr);
    }
    __builtin_unreachable();
}

int pthread_join(pthread_t pthread, void **retval)
{
    // join自己触发死锁
    pthread_t self = pthread_self();
    if(pthread == self)
        return EDEADLK;

    auto internal = pthread_internal(pthread);

    if(internal->routine == nullptr)
    {
        return EPERM;
    }

    if(internal->detachstate == PTHREAD_CREATE_DETACHED)
        return EINVAL;

    auto self_internal = pthread_internal(self);

    // 两个线程相互join触发死锁
    if(self_internal && self_internal->join_thread == pthread)
    {
        return EDEADLK;
    }
    internal->join_thread = self;

    __futex_wait(&internal->joinstate, 0);
    vTaskDelete((TaskHandle_t)pthread);

    if(retval)
        *retval = internal->result;
    delete internal;
    return 0;
}

int pthread_detach(pthread_t pthread)
{
    auto internal = pthread_internal(pthread);
    if(internal->routine == nullptr)
    {
        return EPERM;
    }

    if(internal->detachstate != PTHREAD_CREATE_JOINABLE)
        return EINVAL;
    internal->detachstate = PTHREAD_CREATE_DETACHED;
    return 0;
}

int pthread_getschedparam(pthread_t pthread,
                          int *policy,
                          struct sched_param *param)
{
    if(policy)
        *policy = SCHED_RR;
    if(param)
    {
        memset(param, 0, sizeof(*param));
        param->sched_priority = uxTaskPriorityGet((TaskHandle_t)pthread);
    }
    return 0;
}

int pthread_setschedparam(pthread_t pthread,
                          int policy,
                          const struct sched_param *param)
{
    vTaskPrioritySet((TaskHandle_t)pthread, param->sched_priority);
    return 0;
}

pthread_t pthread_self(void)
{
    return (pthread_t)xTaskGetCurrentTaskHandle();
}

int pthread_equal(pthread_t t1, pthread_t t2)
{
    return t1 == t2;
}

// libstdc++ need it, see __gthread_active_p
int pthread_cancel(pthread_t __pthread)
{
    return EPERM;
}

int pthread_setname_np(pthread_t thread, const char *name)
{
    size_t size = strlen(name);
    if(size >= configMAX_TASK_NAME_LEN)
        return ERANGE;
    
    TaskHandle_t handler = (TaskHandle_t)thread;
    char *task_name = pcTaskGetName(handler);
    strcpy(task_name, name);
    return 0;
}

int pthread_getname_np(pthread_t thread, char name[], size_t size)
{
    TaskHandle_t handler = (TaskHandle_t)thread;
    char *task_name = pcTaskGetName(handler);
    size_t len = strlen(task_name);
    if(len >= size)
        return ERANGE;

    strcpy(name, task_name);
    return 0;
}

extern "C" int main(int argc, const char *argv[]);
extern "C" void __init_main_tls();
extern "C" void __libc_init_array();

static void main_routine(void *args)
{
    atexit([]{taskDISABLE_INTERRUPTS();});
    __libc_init_array();
    static const char *argv[] = {"main", nullptr};
    int argc = sizeof(argv) - 1;
    exit(main(argc, argv));
}

void __start()
{
    __init_main_tls();
    TaskHandle_t handler;
    xTaskCreate(main_routine, "main", 8 * 1024 / sizeof(StackType_t), nullptr, 0, &handler);
    vTaskStartScheduler();
}

extern void * _start() noexcept __attribute__((alias("__start")));

}