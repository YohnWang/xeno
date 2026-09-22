#include<stdlib.h>
#include<xeno/init.h>

extern "C"
{

int main(int argc, const char *argv[]);
void __init_main_tls();
void __libc_init_array();

void __start()
{
    __init_main_tls();
    __libc_init_array();
    static const char *argv[] = {"xeno", nullptr};
    int argc = sizeof(argv) - 1;
    exit(main(argc, argv));
}

void * _start() noexcept __attribute__((alias("__start")));

}