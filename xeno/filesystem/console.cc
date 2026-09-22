#include<errno.h>
#include<string.h>

extern "C" [[gnu::weak]] int __console_read(void *buf, size_t size)
{
    errno = EIO;
    return -1;
}

extern "C" [[gnu::weak]] int __console_write(const void *buf, size_t size)
{
    errno = EIO;
    return -1;
}

extern "C" void __write_terminal(const char *s)
{
    __console_write(s, strlen(s));
}