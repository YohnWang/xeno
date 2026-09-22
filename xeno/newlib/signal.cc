#include<reent.h>

int _kill_r(struct _reent *ptr, int, int)
{
    return -1;
}

extern "C"
[[gnu::weak]]
void sigaltstack()
{

}

extern "C"
[[gnu::weak]]
int sigaction()
{
    return 0;
}