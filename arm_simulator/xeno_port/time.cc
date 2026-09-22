#include<semihost.h>
#include<time.h>

extern "C" 
time_t __get_rtc_time()
{
    return static_cast<time_t>(sys_semihost(SYS_TIME, nullptr));
}