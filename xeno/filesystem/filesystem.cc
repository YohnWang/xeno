#include<xeno/filesystem.h>
#include<xeno/init.h>

namespace xeno
{

void filesystem_init()
{
    std::once_flag flag;
    std::call_once(flag, [](){__vfs.mount(__devfs, "/dev/");});
}

void filesystem_init_stdio()
{
    filesystem_init();
    // TODO 添加/dev/console_input与/dev/console_output是否存在的判断
    
    static stdin_file in;
    static stdout_file out;
    static stderr_file err;
    __devfs.mknod("stdin", &in);
    __devfs.mknod("stdout", &out);
    __devfs.mknod("stderr", &err);
    __vfs.open("/dev/stdin", O_RDWR);
    __vfs.open("/dev/stdout", O_RDWR);
    __vfs.open("/dev/stderr", O_RDWR);
}

}