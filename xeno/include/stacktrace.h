#pragma once

#include<stdint.h>

typedef struct stacktrace_array
{
    uintptr_t a[32];
    int n;
}stacktrace_array;

#ifdef __cplusplus
extern "C" {
#endif

// 打印当前执行点的调用栈
void print_stacktrace();
void store_current_stacktrace(stacktrace_array *parray);
void print_stacktrace_array(const stacktrace_array *parray);

// 打印当前异常捕获时，异常抛出点的调用栈，如果不在捕获块内执行，则行为未定义
void print_current_exception_stacktrace();

#ifdef __cplusplus
}
#endif


#ifdef __cplusplus
inline thread_local stacktrace_array __ext_current_exception_stacktrace;
inline const auto& current_exception_stacktrace() noexcept
{
    return __ext_current_exception_stacktrace;
}
#endif