#include<reent.h>
#include<stdlib.h>
#include<stdint.h>
#include<unistd.h>
#include<utility>
#include<charconv>
#include<iterator>
#include<fcntl.h>

int _getpid_r(struct _reent *)
{
    return 1;
}

[[gnu::weak]]
void _exit(int status)
{
    __write_terminal("progrom exit: ");
    char buf[16] = {};
    std::to_chars(std::begin(buf), std::end(buf), status, 10);
    __write_terminal(buf);
    __write_terminal("\r\n");
    while(1){}
    std::unreachable();
}

int reboot(int op)
{
    int fd = open("/dev/reboot", O_RDWR);
    if(fd == -1)
    {
        errno = EPERM;
        return -1;
    }
    write(fd, &op, sizeof op);
    errno = EINVAL;
    return -1;
}