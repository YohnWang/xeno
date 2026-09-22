#include<stdint.h>
#include<stddef.h>

alignas(16) uint8_t __malloc_buf[12 * 1024 * 1024];
size_t __malloc_buf_size = sizeof(__malloc_buf);