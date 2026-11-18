#include<reent.h>
#include<stdlib.h>
#include<stdint.h>
#include<unistd.h>
#include<errno.h>
#include<xeno/port.h>

// 弱默认定义：端口覆盖 __malloc_buf_size() 时无需提供 __malloc_buf_end；
// 链接脚本用普通赋值即可覆盖（PROVIDE 不会覆盖弱定义）。
[[gnu::weak]]
uint8_t __malloc_buf_end[0];

[[gnu::weak]]
size_t __malloc_buf_size()
{
    return __malloc_buf_end - __malloc_buf_start;
}

extern "C"
void* _sbrk_r(struct _reent *ptr, ptrdiff_t diff)
{
    uint8_t *buf = __malloc_buf_start;
    ptrdiff_t buf_size = (ptrdiff_t)__malloc_buf_size();

    static ptrdiff_t offset = 0;

    if(diff > buf_size - offset || offset + diff < 0)
    {
        errno = ENOMEM;
        return (void*)-1;
    }

    ptrdiff_t pre_offset = offset;
    offset += diff;

    return &buf[pre_offset];
}
