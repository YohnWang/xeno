#pragma once

#include"vfs.h"

#include<flat_map>
#include<mutex>

namespace xeno
{

class devfs;

class devfile: public ifile
{
    friend class devfs;

    std::mutex m;
    int fd = -1;
    int reference_counting = 0;
public:

    ~devfile()
    {
        std::lock_guard _{m};
        if(fd != -1)
        {
            fd_dealloc(fd);
        }
    }

    int open(const char *path, int mode) override
    {
        std::lock_guard _{m};
        if(fd == -1)
        {
            fd = fd_alloc();
        }
        
        if(fd != -1)
            reference_counting++;
        return fd;
    }

    int close() override
    {
        std::lock_guard _{m};
        if(reference_counting > 0)
            reference_counting--;
        if(reference_counting == 0 && fd != -1)
        {
            int f = fd;
            fd = -1;
            return fd_dealloc(f);
        }
        return 0;
    }
};

class devfs: public ifs
{
    friend class devfile;

    std::flat_map<fs::path, devfile*> devfiles;
    std::recursive_mutex mutex;
public:
    using file_type = devfile;

    int mknod(fs::path p, devfile *file)
    {
        std::lock_guard _{mutex};
        p = path_absolute_normal(p);
        if(devfiles.contains(p))
        {
            errno = EBUSY;
            return -1;
        }
        devfiles[p] = file;
        file->vfsptr = this->vfsptr;
        file->ifsptr = this;
        return 0;
    }

    int open(const char *path, int mode) override
    {
        std::lock_guard _{mutex};
        auto p = path_absolute_normal(path);
        if(devfiles.contains(p))
        {
            return devfiles[p]->open(path, mode);
        }
        errno = ENOENT;
        return -1;
    }

    int close(int fd) override
    {
        if(auto f = fget(fd); f)
        {
            return f->close();
        }
        errno = EBADF;
        return -1;
    }
};

}