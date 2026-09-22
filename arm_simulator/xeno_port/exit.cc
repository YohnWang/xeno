#include<semihost.h>
#include<stacktrace.h>
#include<utility>

extern "C"
void _exit(int status)
{
    if(status)
        print_stacktrace();
    struct
    {
        unsigned long reason = 0x20026;
        unsigned long code = -1;
    } arg = {.code = (unsigned long)status};
    sys_semihost(SYS_EXIT_EXTENDED, &arg);
    std::unreachable();
}