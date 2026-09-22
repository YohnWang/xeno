#pragma once

#include<vector>
#include<string>
#include<flat_map>
#include<filesystem>
#include<mutex>
#include<experimental/scope>

#include<unistd.h>
#include<fcntl.h>
#include<sys/stat.h>

namespace xeno
{
namespace fs = std::filesystem;

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


inline fs::path path_absolute_normal(fs::path path)
{
    path = "/" / path;
    return path.lexically_normal();
}

inline fs::path path_relative(const fs::path &path, const fs::path &base)
{
    return path_absolute_normal(path).lexically_relative(path_absolute_normal(base));
}

inline bool path_is_within(const fs::path &path, const fs::path &base)
{
    std::string relative = path_relative(path, base);
    if(relative.empty())
        return true;
    if(relative.size() >= 2 && relative[0] == '.' && relative[1] == '.')
        return false;
    return true;
}

// virtual filesystem
class vfs final
{
    friend class ifs;
    std::flat_map<std::string, ifs*> mountpoints;
    fdallocator fdmap;
    std::recursive_mutex mutex;

    std::tuple<ifs*, std::string> get_ifs_and_prefix(fs::path path)
    {
        std::lock_guard _{mutex};
        
        std::string p = path;
        std::string prefix = {};
        ifs *fs = nullptr;
        for(auto [mp, ifs]: mountpoints)
        {
            if(path_is_within(p, mp))
            {
                if(mp.size() > prefix.size() || fs ==nullptr)
                {
                    prefix = mp;
                    fs = ifs;
                }
            }
        }

        if(fs == nullptr)
            return {};
        auto relative = path_relative(path, prefix);
        relative = path_absolute_normal(relative);
        return {fs, relative};
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

    int mount(ifs &f, fs::path dir)
    {
        if(!dir.is_absolute())
        {
            errno = EINVAL;
            return -1;
        }

        dir = path_absolute_normal(dir);
        std::lock_guard _{mutex};
        if(mountpoints.contains(dir))
        {
            errno = EBUSY;
            return -1;
        }
        mountpoints[dir.string()] = &f;
        f.vfsptr = this;
        return 0;
    }

    int umount(fs::path dir)
    {
        if(!dir.is_absolute())
        {
            errno = EINVAL;
            return -1;
        }

        dir = path_absolute_normal(dir);
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
