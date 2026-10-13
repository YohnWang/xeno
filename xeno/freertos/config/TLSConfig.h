#pragma once

#include <reent.h>
#include <stdint.h>
#include <xeno/predefined.h>

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

static inline void *__init_tls_on_stack(void **ppx_top_of_stack)
{
    size_t size = __get_tls_size();
    size_t align = __get_tls_data_offset();
    uintptr_t top = (uintptr_t)*ppx_top_of_stack;
    char *tp = (char*)((top - size) & ~(uintptr_t)(align - 1));

    *ppx_top_of_stack = tp;
    __init_tls(tp);
    return tp;
}

#define __TLS_BLOCK_FROM_STACK( xTLSBlock, pxTopOfStack )   \
    ( ( xTLSBlock )->tp = __init_tls_on_stack( ( void ** ) &( pxTopOfStack ) ) )

#define configINIT_TLS_BLOCK( xTLSBlock, pxTopOfStack )   \
({\
    struct _tls_block *block = &(xTLSBlock);\
    _reent_tls_init(&(block->reent));\
    __TLS_BLOCK_FROM_STACK(block, pxTopOfStack);\
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
})

#define traceENTER_vTaskDelete( xTaskToDelete ) \
({\
    extern void __pthread_key_destruct(TaskHandle_t);\
    __pthread_key_destruct(xTaskToDelete);\
})

#define traceTASK_CREATE( pxNewTCB ) \
    configASSERT( ( char * ) ( pxNewTCB )->pxTopOfStack >= \
                  ( char * ) ( pxNewTCB )->pxStack )
