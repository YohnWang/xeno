#ifndef __FUTEX_H
#define __FUTEX_H

#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"{
#endif

// 上下文约定：
//   __futex_wait / __futex_timedwait 需要阻塞，只能在任务里调用；
//   __futex_wake / __futex_wake_n / 各 getter 任务和中断都能调用，
//   中断的优先级数值必须 >= configMAX_SYSCALL_INTERRUPT_PRIORITY。
int __futex_wait(int *ptr, int expected);
int __futex_timedwait(int *ptr, int expected, int64_t timeout_millisecond);
void __futex_wake(int *ptr, int value);
int __futex_wake_n(int *ptr, int value, int n);
int __futex_get_wait_numbers(int *ptr);

size_t __futex_get_pool_size();
size_t __futex_get_pool_capacity();

#ifdef __cplusplus
}
#endif

#endif