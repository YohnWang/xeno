#pragma once

#include<stddef.h>

#ifdef __cplusplus
extern "C"{
#endif

void __write_terminal(const char *s);
void __init_tls(void *tp);
size_t __get_tls_size();
void __store_tls_tp(void *tp);
void* __load_tls_tp();

#ifdef __cplusplus
}
#endif