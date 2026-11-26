#pragma once

#include"filesystem/vfs.h"
#include"filesystem/devfs.h"
#include"filesystem/ramfs.h"
#include"filesystem/stdio_file.h"
#include"filesystem/ioctl.h"

namespace xeno
{

void filesystem_init();
void filesystem_init_stdio();

// vfs与devfs需要全局使用，不能进行析构，否则会出现析构完成后，stdio调用close

inline auto& __get_vfs()
{
    alignas(xeno::vfs) static char buf[sizeof(xeno::vfs)];
    static xeno::vfs *ptr = new(buf) xeno::vfs;
    return *ptr;
}

inline auto& __get_devfs()
{
    alignas(xeno::devfs) static char buf[sizeof(xeno::devfs)];
    static xeno::devfs *ptr = new(buf) xeno::devfs;
    return *ptr;
}

inline auto& __get_ramfs()
{
    alignas(xeno::ramfs) static char buf[sizeof(xeno::ramfs)];
    static xeno::ramfs *ptr = new(buf) xeno::ramfs;
    return *ptr;
}

#define __vfs xeno::__get_vfs()
#define __devfs xeno::__get_devfs()
#define __ramfs xeno::__get_ramfs()

}
