#pragma once

#include"filesystem/vfs.h"
#include"filesystem/devfs.h"
#include"filesystem/stdio_file.h"
#include"filesystem/ioctl.h"

namespace xeno
{

void filesystem_init();
void filesystem_init_stdio();

inline auto& __get_vfs()
{
    static xeno::vfs vfs;
    return vfs;
}

inline auto& __get_devfs()
{
    static xeno::devfs devfs;
    return devfs;
}

#define __vfs xeno::__get_vfs()
#define __devfs xeno::__get_devfs()

}
