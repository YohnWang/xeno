#include<semihost.h>
#include<span>
#include<xeno/filesystem.h>
#include<xeno/init.h>

static int get_char()
{
    return sys_semihost(SYS_READC, nullptr);
}

static void put_char(char ch)
{
    sys_semihost(SYS_WRITEC, &ch);
}

extern "C" int __console_read(void *buf, size_t size)
{
    // SYS_READC 会一直阻塞到下一个字符到来，不能在这里循环读满 newlib
    // 请求的整个缓冲区，否则单行输入要等缓冲区填满才会交给调用者。
    if(size == 0)
        return 0;
    int c = get_char();
    if(c == -1)
        return 0;
    *static_cast<char *>(buf) = (char)c;
    return 1;
}


extern "C" [[gnu::used]] int __console_write(const void *buf, size_t size)
{
    const char *s = static_cast<const char *>(buf);
    std::span str{s, size};
    for(auto c: str)
    {
        put_char(c);
    }
    return size;
}

static void init()
{
    xeno::filesystem_init();
    static xeno::console_input_file input;
    static xeno::console_output_file<> output;
    __devfs.mknod("console_input", &input);
    __devfs.mknod("console_output", &output);
    xeno::filesystem_init_stdio();
}

__sys_init(init);