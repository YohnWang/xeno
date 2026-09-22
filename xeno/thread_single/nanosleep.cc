#include<unistd.h>
#include<errno.h>
#include<time.h>
#include<stdint.h>

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

    struct timespec start, target, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    target.tv_sec  = start.tv_sec + req->tv_sec;
    target.tv_nsec = start.tv_nsec + req->tv_nsec;
    if (target.tv_nsec >= 1000000000L)
    {
        target.tv_sec  += 1;
        target.tv_nsec -= 1000000000L;
    }

    for (;;)
    {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > target.tv_sec || (now.tv_sec == target.tv_sec && now.tv_nsec >= target.tv_nsec))
            break;
    }

    if (rem)
    {
        rem->tv_sec  = 0;
        rem->tv_nsec = 0;
    }

    return 0;
}
