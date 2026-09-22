#include<reent.h>
#include<stdlib.h>
#include<stdint.h>
#include<unistd.h>
#include<stacktrace.h>
#include<charconv>
#include<xeno/port.h>

extern "C"
void* _sbrk_r(struct _reent *ptr, ptrdiff_t diff)
{
    uint8_t *buf = __malloc_buf;
    ptrdiff_t buf_size = (ptrdiff_t)__malloc_buf_size;

    static ptrdiff_t offset = 0;

    if(offset + diff >= buf_size || offset < 0)
    {
        return (void*)-1;
    }

    ptrdiff_t pre_offset = offset;
    offset += diff;

    return &buf[pre_offset];
}
