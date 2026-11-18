#pragma once

#include<stddef.h>

#ifdef __cplusplus
extern "C"{
#endif

// io
int __console_read(void *buf, size_t size);
int __console_try_read(void *buf, size_t size);
int __console_write(const void *buf, size_t size);

// machine
bool __irq_disable();
void __irq_enable();
bool __is_interrupted();

// os
bool __scheduler_suspend();
void __scheduler_resume();


extern uint8_t __malloc_buf_start[];
extern uint8_t __malloc_buf_end[]; // 弱默认定义，链接脚本可用普通赋值覆盖
extern size_t __malloc_buf_size(); // 弱默认按 start/end 计算，端口可强定义覆盖

#ifdef __cplusplus
}
#endif