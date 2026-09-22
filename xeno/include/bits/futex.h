#ifndef __FUTEX_H
#define __FUTEX_H

#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"{
#endif

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