#pragma once

#include<stddef.h>

#ifdef __cplusplus
extern "C"{
#endif

// io
int __console_read(void *buf, size_t size);
int __console_write(const void *buf, size_t size);

// machine
bool __irq_disable();
void __irq_enable();

// os
bool __scheduler_suspend();
void __scheduler_resume();


extern uint8_t __malloc_buf[];
extern size_t  __malloc_buf_size;

#ifdef __cplusplus
}
#endif