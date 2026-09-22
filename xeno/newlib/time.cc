#include<xeno/filesystem.h>
#include<time.h>
#include<unistd.h>
#include<reent.h>

extern "C"
{

static int get_rtc_fd()
{
    static int fd = open("/dev/rtc", O_RDWR);
    return fd;
}

[[gnu::weak]]
time_t __get_rtc_time()
{
    time_t time = 0;
    int n = read(get_rtc_fd(), &time, sizeof time);
    return time;
}

[[gnu::weak]]
void __set_rtc_time(time_t time)
{
    int n = write(get_rtc_fd(), &time, sizeof time);
}

static int get_systick_fd()
{
    static int fd = open("/dev/systick", O_RDWR);
    return fd;
}

[[gnu::weak]]
time_t __get_systick()
{
    time_t time = 0;
    int n = read(get_systick_fd(), &time, sizeof time);
    return time;
}

static auto& last_rtc_time_()
{
    static time_t last_rtc_time = __get_rtc_time();
    return last_rtc_time;
}
#define last_rtc_time last_rtc_time_()

static auto& last_steady_time_()
{
    static time_t last_steady_time = __get_systick();
    return last_steady_time;
}
#define last_steady_time last_steady_time_()

int clock_settime(clockid_t clock_id, const struct timespec *tp)
{
    if(clock_id == CLOCK_MONOTONIC)
    {
        return -1;
    }
    else if(clock_id == CLOCK_REALTIME)
    {
        __set_rtc_time(tp->tv_sec);
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        
        last_rtc_time = tp->tv_sec;
        last_steady_time = __get_systick();
        
        return 0;
    }
    return -1;
}

int clock_gettime(clockid_t clock_id, struct timespec *tp)
{
    if(clock_id == CLOCK_MONOTONIC)
    {
        time_t ms = __get_systick();
        tp->tv_sec = ms / 1000;
        tp->tv_nsec = (ms % 1000) * 1000 *1000;
        return 0;
    }
    else if(clock_id == CLOCK_REALTIME)
    {
        struct timespec t = {};
        clock_gettime(CLOCK_MONOTONIC, &t);
        t.tv_sec += last_rtc_time - last_steady_time;
        *tp = t;
        return 0;
    }
    return -1;
}

int _gettimeofday_r(struct _reent *ptr, struct timeval * tv, void * tz)
{
    struct timespec t = {};
    clock_gettime(CLOCK_REALTIME, &t);
    tv->tv_sec = t.tv_sec;
    tv->tv_usec = t.tv_nsec / 1000;
    return 0;
}

}