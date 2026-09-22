#pragma once
#include"kits.h"

#define __xeno_sys_name(name) __xeno_macro_cat(__xeno_macro_cat(__preinit_, name), __xeno_macro_cat(__LINE__, __COUNTER__))

typedef void (*__preinit_function_t)();
#define __sys_init(function) [[gnu::section(".preinit_array"), gnu::used]] __preinit_function_t __xeno_sys_name(function) = function

#ifdef __cplusplus
extern "C"{
#endif

void __init_main_tls();

#ifdef __cplusplus
}
#endif