#include<unistd.h>
#include<FreeRTOS.h>
#include<task.h>
#include<errno.h>
#include<time.h>
#include<stdint.h>
#include<numeric>

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    if (req == NULL || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L)
    {
        errno = EINVAL;
        return -1;
    }

    if (req->tv_sec == 0 && req->tv_nsec == 0)
    {
        if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
        return 0;
    }

    constexpr auto max_ms = pdTICKS_TO_MS(std::numeric_limits<TickType_t>::max());
    int64_t ms = req->tv_sec * 1000LL + req->tv_nsec/1000/1000;
    if(ms > max_ms)
    {
        errno = EINVAL;
        return -1;
    }

    TickType_t tick = pdMS_TO_TICKS(ms);
    vTaskDelay(tick);

    if (rem)
    {
        rem->tv_sec  = 0;
        rem->tv_nsec = 0;
    }

    return 0;
}
