#include<sys/lock.h>
#include<pthread.h>
#include<stdlib.h>
#include<xeno/kits.h>

#include<FreeRTOS.h>
#include<semphr.h>

struct __lock
{
    pthread_mutex_t m;
    bool unused;
};

extern "C"
{

struct __lock xeno_link_wrap(__lock___sinit_recursive_mutex) = {PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP};
struct __lock xeno_link_wrap(__lock___sfp_recursive_mutex) = {PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP};
struct __lock xeno_link_wrap(__lock___atexit_recursive_mutex) = {PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP};
struct __lock xeno_link_wrap(__lock___at_quick_exit_mutex) = {PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP};
struct __lock xeno_link_wrap(__lock___malloc_recursive_mutex) = {.m = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP , .unused = true};
struct __lock xeno_link_wrap(__lock___env_recursive_mutex) = {PTHREAD_MUTEX_INITIALIZER};
struct __lock xeno_link_wrap(__lock___tz_mutex) = {PTHREAD_MUTEX_INITIALIZER};
struct __lock xeno_link_wrap(__lock___dd_hash_mutex) = {PTHREAD_MUTEX_INITIALIZER};
struct __lock xeno_link_wrap(__lock___arc4random_mutex) = {PTHREAD_MUTEX_INITIALIZER};

void xeno_link_wrap(__retarget_lock_init)(_LOCK_T *lock)
{
    if(lock == nullptr)
        return;
    
    *lock = (struct __lock*)malloc(sizeof(struct __lock));
    (*lock)->m = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    (*lock)->unused = false;
}

// 2. 初始化递归锁（常用：malloc/env/stdio/atexit）
void xeno_link_wrap(__retarget_lock_init_recursive)(_LOCK_T *lock)
{
    if(lock == nullptr)
        return;
    
    *lock = (struct __lock*)malloc(sizeof(struct __lock));
    (*lock)->m = (pthread_mutex_t)PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
    (*lock)->unused = false;
}

// 3. 销毁锁
void xeno_link_wrap(__retarget_lock_close)(_LOCK_T lock)
{
    if(lock == nullptr)
        return;
    
    pthread_mutex_destroy(&lock->m);
    free(lock);
}

void xeno_link_wrap(__retarget_lock_close_recursive)(_LOCK_T lock)
{
    if(lock == nullptr)
        return;
    
    pthread_mutex_destroy(&lock->m);
    free(lock);
}

// 4. 加锁（阻塞）
void xeno_link_wrap(__retarget_lock_acquire)(_LOCK_T lock)
{
    if(lock->unused == true)
        vTaskSuspendAll();
    else
        pthread_mutex_lock(&lock->m);
}

void xeno_link_wrap(__retarget_lock_acquire_recursive)(_LOCK_T lock)
{
    if(lock->unused == true)
        vTaskSuspendAll();
    else
        pthread_mutex_lock(&lock->m);
}

// 5. 尝试加锁（不阻塞，返回 0=成功）
int xeno_link_wrap(__retarget_lock_try_acquire)(_LOCK_T lock)
{
    if(lock->unused == true)
    {
        vTaskSuspendAll();
        return 0;
    }
    return pthread_mutex_trylock(&lock->m);
}

int xeno_link_wrap(__retarget_lock_try_acquire_recursive)(_LOCK_T lock)
{
    if(lock->unused == true)
    {
        vTaskSuspendAll();
        return 0;
    }
    return pthread_mutex_trylock(&lock->m);
}

// 6. 解锁
void xeno_link_wrap(__retarget_lock_release)(_LOCK_T lock)
{
    if(lock->unused == true)
        xTaskResumeAll();
    else
        pthread_mutex_unlock(&lock->m);
}

void xeno_link_wrap(__retarget_lock_release_recursive)(_LOCK_T lock)
{
    if(lock->unused == true)
        xTaskResumeAll();
    else
        pthread_mutex_unlock(&lock->m);
}

}