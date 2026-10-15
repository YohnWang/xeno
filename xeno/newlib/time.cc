#include<xeno/filesystem.h>
#include<time.h>
#include<unistd.h>
#include<reent.h>
#include<errno.h>
#include<stdint.h>

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

static constexpr int64_t ns_per_sec = 1000000000LL;

static constexpr int64_t to_ns(const struct timespec &t)
{
    return (int64_t)t.tv_sec * ns_per_sec + t.tv_nsec;
}

static struct timespec from_ns(int64_t ns)
{
    int64_t sec  = ns / ns_per_sec;
    int64_t nsec = ns % ns_per_sec;
    if(nsec < 0)        // tv_nsec 必须落在 [0, 1e9)
    {
        nsec += ns_per_sec;
        sec  -= 1;
    }

    struct timespec t;
    t.tv_sec  = sec;
    t.tv_nsec = nsec;
    return t;
}

// CLOCK_REALTIME = CLOCK_MONOTONIC + offset。
// offset 的单位必须是纳秒：__get_systick() 量的是毫秒，而 tv_sec 是秒，
// 拿毫秒去减秒会跑飞。第一次使用时按当前的 RTC 和 MONOTONIC 算出来。
static int64_t& realtime_offset()
{
    static int64_t offset = []{
        struct timespec mono = {};
        clock_gettime(CLOCK_MONOTONIC, &mono);
        return (int64_t)__get_rtc_time() * ns_per_sec - to_ns(mono);
    }();
    return offset;
}

int clock_settime(clockid_t clock_id, const struct timespec *tp)
{
    if(clock_id != CLOCK_REALTIME)
    {
        errno = EINVAL;
        return -1;
    }

    __set_rtc_time(tp->tv_sec);

    struct timespec mono = {};
    clock_gettime(CLOCK_MONOTONIC, &mono);
    realtime_offset() = to_ns(*tp) - to_ns(mono);
    return 0;
}

int clock_gettime(clockid_t clock_id, struct timespec *tp)
{
    if(clock_id == CLOCK_MONOTONIC)
    {
        time_t ms = __get_systick();
        tp->tv_sec = ms / 1000;
        tp->tv_nsec = (ms % 1000) * 1000 * 1000;
        return 0;
    }
    else if(clock_id == CLOCK_REALTIME)
    {
        struct timespec mono = {};
        clock_gettime(CLOCK_MONOTONIC, &mono);
        *tp = from_ns(to_ns(mono) + realtime_offset());
        return 0;
    }

    errno = EINVAL;
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