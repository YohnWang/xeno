#pragma once
#include<stddef.h>
#include_next<pthread.h>


#ifdef __cplusplus
extern "C"{
#endif

// newlib 没有提供这个 POSIX 常量（按 POSIX 应该在 <limits.h> 里）：
// pthread 退出时析构函数最多被调用的轮数
#ifndef PTHREAD_DESTRUCTOR_ITERATIONS
#define PTHREAD_DESTRUCTOR_ITERATIONS 4
#endif

void set_next_thread_name(const char *name);
void set_next_thread_stack(void *ptr, size_t bytes);
void set_next_thread_stack_size(size_t bytes);
void set_next_thread_priority(int priority);
int pthread_setname_np(pthread_t thread, const char *name);
int pthread_getname_np(pthread_t thread, char name[], size_t size);

#ifdef __cplusplus
}
#endif