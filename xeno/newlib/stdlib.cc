#include<reent.h>
#include<stdlib.h>
#include<xeno/shell.h>

int _system_r(struct _reent *, const char *cmd)
{
    if(cmd == nullptr)
        return 1;
    return xeno::shell_system(cmd);
}

int system(const char *cmd)
{
    return _system_r(_REENT, cmd);
}