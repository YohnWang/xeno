#include<stdint.h>
#include<stddef.h>
#include<xeno/port.h>

alignas(16) uint8_t __malloc_buf_start[12 * 1024 * 1024];

extern "C"
size_t __malloc_buf_size()
{
    return sizeof(__malloc_buf_start);
}
