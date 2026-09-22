#include<stdlib.h>
#include<xeno/init.h>

extern "C"
{

[[gnu::used]]
void *__aeabi_tp;

[[gnu::naked]]
void * __aeabi_read_tp()
{
    __asm__ volatile (R"(
        ldr r0,=__aeabi_tp
        ldr r0,[r0]
        bx lr
    )");
}

void __store_tls_tp(void *tp)
{
    __atomic_store_n(&__aeabi_tp, tp, __ATOMIC_RELEASE);
}

void* __load_tls_tp()
{
    return (void*)__atomic_load_n(&__aeabi_tp, __ATOMIC_ACQUIRE);
}

// support for thread local storage
extern char __tdata_start[];
extern char __tdata_end[];
extern char __tbss_start[];
extern char __tbss_end[];

static constexpr int TCB_size = 8;

size_t __get_tls_size()
{
    size_t tdata_size = __tdata_end - __tdata_start;
    size_t tbss_size  = __tbss_end - __tbss_start;
    size_t t_size = tdata_size + tbss_size;
    return t_size + TCB_size;
}

void __init_tls(void *tp)
{
    size_t tdata_size = __tdata_end - __tdata_start;
    size_t tbss_size  = __tbss_end - __tbss_start;
    size_t t_size = tdata_size + tbss_size;
    char *tdata_start_ptr = (char*)tp + TCB_size;
    char *tbss_start_ptr = tdata_start_ptr + tdata_size;
    __builtin_memcpy(tdata_start_ptr, __tdata_start, tdata_size);
    __builtin_memset(tbss_start_ptr, 0, tbss_size);
}

void __init_main_tls()
{
    size_t n = __get_tls_size();
    if(n == 0)
        return;

    void *p = malloc(n);
    if(p == nullptr)
    {
        __write_terminal("__init_main_tls malloc failed\r\n");
        abort();
    }
    __init_tls(p);
    __store_tls_tp(p);
}

}