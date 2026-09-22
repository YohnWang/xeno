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
    char *s = static_cast<char *>(buf);
    size_t i;
    for(i = 0; i < size; i++)
    {
        int c = get_char();
        if(c == -1)
            break;
        s[i] = (char)c;
    }
    return i;
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