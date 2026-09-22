#pragma once

#include <reent.h>

#define configUSE_C_RUNTIME_TLS_SUPPORT    1

struct _tls_block
{
#ifndef _REENT_THREAD_LOCAL
    struct _reent reent;
#endif
    void *tp;
};

#define configTLS_BLOCK_TYPE           struct _tls_block

#ifndef _REENT_THREAD_LOCAL
#define _reent_tls_init(reent) _REENT_INIT_PTR( reent )
#define _reent_set(reent) (_impure_ptr = reent)
#define _reent_deinit(reent) _reclaim_reent( reent )
#else
#define _reent_tls_init(reent) 
#define _reent_set(reent) 
#define _reent_deinit(reent) 
#endif

#define configINIT_TLS_BLOCK( xTLSBlock, pxTopOfStack )   \
({\
    struct _tls_block *block = &(xTLSBlock);\
    _reent_tls_init(&(block->reent));\
    size_t t_size = __get_tls_size();\
    void *tpmem = pvPortMalloc(t_size);\
    __init_tls(tpmem);\
    block->tp = tpmem;\
})


#define configSET_TLS_BLOCK( xTLSBlock )   \
({\
    struct _tls_block *block = &(xTLSBlock);\
    _reent_set(&(block->reent));\
    __store_tls_tp(block->tp);\
})

#define configDEINIT_TLS_BLOCK( xTLSBlock )   \
({\
    struct _tls_block *block = &(xTLSBlock);\
    _reent_deinit(&(block->reent));\
    vPortFree(block->tp);\
})

#define traceENTER_vTaskDelete( xTaskToDelete ) \
({\
    extern void __pthread_key_destruct(TaskHandle_t);\
    __pthread_key_destruct(xTaskToDelete);\
})
