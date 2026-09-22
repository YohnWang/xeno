#pragma once

#include"devfs.h"
#include<xeno/kits.h>
#include<semaphore>
#include<span>

#include"ioctl.h"

namespace xeno
{

class stdin_file: public devfile
{
    int fd = -1;
public:

    int open(const char *path, int mode) override
    {
        fd = vfs().open("/dev/console_input", O_WRONLY);
        if(fd < 0)
            return fd;
        return fd_alloc(STDIN_FILENO);
    }

    int close() override
    {
        int r = vfs().close(fd);
        if(r != 0)
            return r;
        return fd_dealloc(STDIN_FILENO);
    }

    int read(void *buf, size_t size) override
    {
        return vfs().read(fd, buf, size);
    }
};

class stdout_file: public devfile
{
    int fd = -1;
public:

    int open(const char *path, int mode) override
    {
        fd = vfs().open("/dev/console_output", O_WRONLY);
        if(fd < 0)
            return fd;
        return fd_alloc(STDOUT_FILENO);
    }

    int close() override
    {
        int r = vfs().close(fd);
        if(r != 0)
            return r;
        return fd_dealloc(STDOUT_FILENO);
    }

    int write(const void *buf, size_t size) override
    {
        return vfs().write(fd, buf, size);
    }
};

class stderr_file: public devfile
{
    int fd = -1;
public:

    int open(const char *path, int mode) override
    {
        fd = vfs().open("/dev/console_output", O_WRONLY);
        if(fd < 0)
            return fd;
        return fd_alloc(STDERR_FILENO);
    }

    int close() override
    {
        int r = vfs().close(fd);
        if(r != 0)
            return r;
        return fd_dealloc(STDERR_FILENO);
    }

    int write(const void *buf, size_t size) override
    {
        return vfs().write(fd, buf, size);
    }
};

extern "C" int __console_read(void *buf, size_t size);
extern "C" int __console_write(const void *buf, size_t size);

class console_input_file: public devfile
{
    int fd;
public:

    int open(const char *path, int mode) override
    {
        fd = fd_alloc();
        return fd;
    }

    int close() override
    {
        return fd_dealloc(fd);
    }

    int read(void *buf, size_t size) override
    {
        return __console_read(buf, size);
    }
};

template<size_t N = 0>
class console_output_file: public devfile
{
    struct buffer
    {
        static_queue<char, N> queue;
        std::counting_semaphore<> full{0};
        std::counting_semaphore<> empty{N};
        std::mutex mutex;

        void putchar(int ch)
        {
            if(empty.try_acquire())
            {
                std::lock_guard _{mutex};
                queue.push_overwrite(ch);
                full.release();
            }
            else
            {
                std::lock_guard _{mutex};
                queue.push_overwrite(ch);
            }
        }

        int getchar()
        {
            int ch = EOF;
            full.acquire();
            {
                std::lock_guard _{mutex};
                char c;
                if(queue.pop(c))
                    ch = c;
            }
            empty.release();
            return ch;
        }

        int try_getchar()
        {
            int ch = EOF;
            if(full.try_acquire())
            {
                {
                    std::lock_guard _{mutex};
                    char c;
                    if(queue.pop(c))
                        ch = c;
                }
                empty.release();
            }
            return ch;
        }
    };

    std::conditional_t<(N > 0), buffer, uint8_t> printbuffer;
    bool enable = true;
    int fd;
public:

    int open(const char *path, int mode) override
    {
        fd = fd_alloc();
        return fd;
    }

    int close() override
    {
        return fd_dealloc(fd);
    }

    int read(void *buf, size_t size) override
    {
        if constexpr(N > 0)
        {
            auto a = static_cast<char*>(buf);
            if(size == 0)
                return 0;
            a[0] = printbuffer.getchar();
            size_t i = 1;
            char c;
            while(i < size && (c = printbuffer.try_getchar()) != EOF)
                a[i++] = c;
            return i;
        }
        return -1;
    }

    int write(const void *buf, size_t size) override
    {
        if constexpr(N > 0)
        {
            auto a = static_cast<const char*>(buf);
            for(size_t i = 0; i < size; i++)
                printbuffer.putchar(a[i]);
        }
        std::span a{(const char*)buf, size};
        int sum = 0;
        for(auto i: a)
        {
            if(i == '\n')
                __console_write("\r", 1);
            if(__console_write(&i, 1) == 1)
                sum++; 
        }
        return sum;
    }

    // struct ctrl: ioctlor<ctrl>
    // {
    //     using ioctlor<ctrl>::parent;
        
    //     int operator()(bool e)
    //     {
    //         parent.enable = e;
    //         return 0;
    //     }
    // }_ctrl{*this};
};

}