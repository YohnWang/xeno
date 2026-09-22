#pragma once

#include<stddef.h>
#include<pthread.h>

#ifdef __cplusplus
extern "C"{
#endif

void set_next_thread_name(const char *name);
void set_next_thread_stack_size(size_t bytes);
void set_next_thread_priority(int priority);
int pthread_setname_np(pthread_t thread, const char *name);
int pthread_getname_np(pthread_t thread, char name[], size_t size);

#ifdef __cplusplus
}
#endif