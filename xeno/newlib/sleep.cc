#include<unistd.h>
#include<errno.h>
#include<time.h>

unsigned int sleep(unsigned int seconds)
{
    struct timespec ts;
    ts.tv_sec  = (time_t)seconds;
    ts.tv_nsec = 0;
    nanosleep(&ts, nullptr);
    return 0;
}
extern "C"
int usleep(useconds_t usec)
{
    if(usec >= 1000000)
    {
        errno = EINVAL;
        return -1;
    }
    struct timespec ts;
    ts.tv_sec  = 0;
    ts.tv_nsec = (long)usec * 1000L;
    return nanosleep(&ts, nullptr);
}