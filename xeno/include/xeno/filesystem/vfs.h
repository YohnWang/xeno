#pragma once

#include<vector>
#include<string>
#include<flat_map>
#include<mutex>
#include<experimental/scope>

#include<unistd.h>
#include<fcntl.h>
#include<sys/stat.h>
#include<errno.h>

#include"path.h"

namespace xeno
{

class vfs;

// interface filesystem
class ifs
{
    friend class vfs;
protected:
    xeno::vfs *vfsptr = nullptr;
public:

    auto& vfs(this auto &&self)
    {
        return *self.vfsptr;
    }

    auto fget(this auto &&self, int fd)
    {
        return static_cast<std::remove_cvref_t<decltype(self)>::file_type*>(self.vfs().fget(fd));
    }

    virtual int open(const char *path, int mode) = 0;
    virtual int close(int fd) = 0;

    virtual int mkdir(const char *path, mode_t mode)
    {
        return -1;
    }

    virtual int rename(const char *oldpath, const char *newpath)
    {
        errno = EACCES;
        return -1;
    }
};

class ifile
{
    friend class vfs;
protected:
    xeno::vfs *vfsptr = nullptr;
    auto& vfs(this auto &&self)
    {
        if(self.vfsptr == nullptr)
            self.vfsptr = &self.ifs().vfs();
        return *self.vfsptr;
    }

    xeno::ifs *ifsptr = nullptr;
    auto& ifs(this auto &&self)
    {
        return *self.ifsptr;
    }

    int fd_alloc(this auto &&self)
    {
        return self.vfs().fd_alloc(&self);
    }

    int fd_alloc(this auto &&self, int fd)
    {
        return self.vfs().fd_alloc(&self, fd);
    }

    int fd_dealloc(this auto &&self, int fd)
    {
        return self.vfs().fd_dealloc(fd);
    }
public:

    ifile() = default;
    ifile(xeno::ifs *pifs):ifsptr{pifs}{}

    virtual int open(const char *path, int mode) = 0;
    virtual int close() = 0;

    virtual int read(void *buf, size_t size)
    {
        errno = EIO;
        return -1;
    }

    virtual int write(const void *buf, size_t size)
    {
        errno = EIO;
        return -1;
    }

    virtual off_t lseek(off_t offset, int whence)
    {
        errno = EOVERFLOW;
        return -1;
    }

    virtual int truncate(const char *, off_t length)
    {
        errno = EPERM;
        return -1;
    }

    virtual int sync()
    {
        return 0;
    }

    virtual int fstat(struct stat *buf)
    {
        errno = EIO;
        return -1;
    }
};

// open file description
class ofd
{
    static constexpr size_t limits = 1024;
    std::vector<ifile*> v;

    int expand(size_t n)
    {
        if(v.size() >= limits)
        {
            errno = ENFILE;
            return -1;
        }

        size_t size = std::max(n, v.size() + v.size()/2);
        size = std::min(size, limits);
        v.resize(size, nullptr);
        return 0;
    }

public:
    int allocate(ifile *f, int fd)
    {
        if(fd < 0 || (size_t)fd >= limits)
        {
            errno = EBADF;
            return -1;
        }

        if((size_t)fd < v.size())
        {
            if(v[fd] == nullptr)
            {
                v[fd] = f;
                return fd;
            }
            errno = EBUSY;
            return -1;
        }

        if(expand((size_t)fd + 1) == -1)
            return -1;

        if((size_t)fd >= v.size())
        {
            errno = EMFILE;
            return -1;
        }

        v[fd] = f;
        return fd;
    }

    int allocate(ifile *f)
    {
        size_t i;
        for(i = 3; i < v.size(); i++)
        {
            if(v[i] == nullptr)
            {
                v[i] = f;
                return (int)i;
            }
        }
        if(expand(i + 1) == -1)
            return -1;
        v[i] = f;
        return (int)i;
    }

    int deallocate(int fd)
    {
        if(fd >= 0 && (size_t)fd < v.size())
        {
            v[fd] = nullptr;
            return 0;
        }
        errno = EBADF;
        return -1;
    }

    ifile* operator[](int fd)
    {
        if(fd >= 0 && (size_t)fd < v.size())
        {
            return v[fd];
        }
        return nullptr;
    }
};

class fdallocator
{
    std::flat_map<int, ifile*> pool;
    int fd = 1000000;
public:

    int allocate(ifile *fs, int fd)
    {
        if(pool.contains(fd))
            return -1;
        pool[fd] = fs;
        return fd;
    }

    int allocate(ifile *fs)
    {
        int r = fd++;
        return allocate(fs, r);
    }

    int deallocate(int fd)
    {
        auto r = pool.erase(fd);
        if(r == 0)
            return -1;
        return 0;
    }

    ifile* operator[](int fd)
    {
        if(pool.contains(fd))
            return pool[fd];
        return nullptr;
    }
};

// virtual filesystem
class vfs final
{
    friend class ifs;
    std::flat_map<path, ifs*> mountpoints;
    ofd fdmap;
    std::recursive_mutex mutex;

    std::tuple<ifs*, std::string> get_ifs_and_prefix(path p)
    {
        std::lock_guard _{mutex};
        std::string_view path_str = p;
        std::string_view prefix = {};
        ifs *fs = nullptr;

        for(auto &&[mp, f]: mountpoints)
        {
            std::string_view mp_str = mp;
            if(mp_str.size() < prefix.size() || mp_str.size() > path_str.size())
                continue;
            if(path_str.compare(0, mp_str.size(), mp_str) != 0)
                continue;
            if(mp_str.size() < path_str.size() && mp_str.back() != '/' && path_str[mp_str.size()] != '/')
                continue;

            prefix = mp_str;
            fs = f;
        }

        if(fs == nullptr)
            return {};

        std::string_view relative = prefix == "/" ? path_str : path_str.substr(prefix.size());
        if(relative.empty())
            relative = "/";
        return {fs, std::string(relative)};
    }

    auto get_ifile(int fd)
    {
        std::lock_guard _{mutex};
        return fdmap[fd];
    }
public:

    std::string get_mountpoint(ifs *fs)
    {
        for(auto [mp, ifs]: mountpoints)
        {
            if(ifs == fs)
                return mp;
        }
        return "";
    }

    int fd_alloc(ifile *file)
    {
        std::lock_guard _{mutex};
        return fdmap.allocate(file);
    }

    int fd_alloc(ifile *file, int fd)
    {
        std::lock_guard _{mutex};
        return fdmap.allocate(file, fd);
    }

    int fd_dealloc(int fd)
    {
        std::lock_guard _{mutex};
        return fdmap.deallocate(fd);
    }

    int mount(ifs &f, path dir)
    {
        std::lock_guard _{mutex};
        if(mountpoints.contains(dir))
        {
            errno = EBUSY;
            return -1;
        }
        mountpoints[dir] = &f;
        f.vfsptr = this;
        return 0;
    }

    int umount(path dir)
    {
        std::lock_guard _{mutex};
        if(mountpoints.contains(dir))
        {
            mountpoints.erase(dir);
            return 0;
        }
        errno = ENOENT;
        return -1;
    }

    ifile* fget(int fd)
    {
        return get_ifile(fd);
    }

    int open(const char *path, int flags)
    {
        if(path != nullptr)
        {
            auto [fs, relative] = get_ifs_and_prefix(path);
            if(fs)
            {
                return fs->open(relative.c_str(), flags);
            }
        }

        errno = ENOENT;
        return -1;
    }

    int read(int fd, void *buf, size_t size)
    {
        ifile *file = get_ifile(fd);
        if(file)
        {
            return file->read(buf, size);
        }
        errno = EBADF;
        return -1;
    }

    int write(int fd, const void *buf, size_t size)
    {
        ifile *file = get_ifile(fd);
        if(file)
        {
            return file->write(buf, size);
        }
        errno = EBADF;
        return -1;
    }

    int close(int fd)
    {
        ifile *file = get_ifile(fd);
        if(file)
        {
            return file->ifs().close(fd);
        }
        errno = EBADF;
        return -1;
    }

    int lseek(int fd, off_t offset, int whence)
    {
        ifile *file = get_ifile(fd);
        if(file)
        {
            return file->lseek(offset, whence);
        }
        errno = EBADF;
        return -1;
    }

    int fstat(int fd, struct stat *buf)
    {
        ifile *file = get_ifile(fd);
        if(file)
        {
            return file->fstat(buf);
        }
        errno = EBADF;
        return -1;
    }

    int stat(const char *path, struct stat *buf)
    {
        int fd = open(path, O_RDONLY);
        if(fd < 0)
            return fd;
        std::experimental::scope_exit _{[fd, this]{
            close(fd);
        }};
        return fstat(fd, buf);
    }

};

}
