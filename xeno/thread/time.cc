#include<FreeRTOS.h>
#include<task.h>
#include<time.h>

#include<xeno/filesystem.h>

using namespace xeno;

class systick: public devfile
{
    int fd;
public:

    int open(const char *path, int mode) override
    {
        fd = fd_alloc();
        return fd;
    }

    int close() override
    {
        return fd_dealloc(fd);
    }

    int read(void *buf, size_t size) override
    {
        if(size < sizeof(time_t))
        {
            errno = EINVAL;
            return -1;
        }
        
        auto tick = xTaskGetTickCount();
        time_t t = pdTICKS_TO_MS(tick);
        __builtin_memcpy(buf, &t, sizeof(t));
        return (int)sizeof(t);
    }
};

#pragma GCC diagnostic ignored "-Wprio-ctor-dtor"

[[gnu::constructor(0)]]
static void systick_init()
{
    static systick tick;
    __devfs.mknod("systick", &tick);
}
