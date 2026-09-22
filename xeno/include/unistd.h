#pragma once

#include_next<unistd.h>

#ifdef __cplusplus
extern "C"{
#endif

int reboot(int op);
int usleep(useconds_t usec);
long int syscall (long int __sysno, ...);

#ifdef __cplusplus
}
#endif