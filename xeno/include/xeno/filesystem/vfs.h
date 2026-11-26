#pragma once

#include<algorithm>
#include<vector>
#include<string>
#include<tuple>
#include<flat_map>
#include<mutex>
#include<new>

#include<unistd.h>
#include<fcntl.h>
#include<sys/stat.h>
#include<sys/dirent.h>
#include<errno.h>

#include"path.h"

namespace xeno
{

class vfs;

// C++ 层和 C 层共用同一个 struct dirent
using dirent = struct ::dirent;

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

    virtual int open(const char *path, int mode) = 0;
    virtual int close(int fd) = 0;

    virtual int mkdir(const char *path, mode_t mode)
    {
        errno = EROFS;
        return -1;
    }

    virtual int rmdir(const char *path)
    {
        errno = EROFS;
        return -1;
    }

    virtual int unlink(const char *path)
    {
        errno = EROFS;
        return -1;
    }

    virtual int rename(const char *oldpath, const char *newpath)
    {
        errno = EROFS;
        return -1;
    }

    // 默认实现：open + fstat + close；有更直接办法的文件系统应当覆盖。
    // 定义放在 vfs 之后（这里 vfs 还是前置声明）。
    virtual int stat(const char *path, struct stat *buf);
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
    virtual ~ifile() = default;

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

    // 目录遍历：1=取到一项，0=目录读完，-1=错误。
    // 游标属于本打开描述符；实现负责填 entry.d_ino/d_type/d_name。
    virtual int readdir(xeno::dirent &entry)
    {
        errno = ENOTDIR;
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

// virtual filesystem
class vfs final
{
    friend class ifs;
    std::flat_map<path, ifs*> mountpoints;
    std::mutex mount_mutex;
    ofd fdmap;
    std::mutex fd_mutex;

    std::tuple<ifs*, std::string> get_ifs_and_prefix(path p)
    {
        std::lock_guard _{mount_mutex};
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
        std::lock_guard _{fd_mutex};
        return fdmap[fd];
    }
public:

    int fd_alloc(ifile *file)
    {
        std::lock_guard _{fd_mutex};
        return fdmap.allocate(file);
    }

    int fd_alloc(ifile *file, int fd)
    {
        std::lock_guard _{fd_mutex};
        return fdmap.allocate(file, fd);
    }

    int fd_dealloc(int fd)
    {
        std::lock_guard _{fd_mutex};
        return fdmap.deallocate(fd);
    }

    int mount(ifs &f, path dir)
    {
        std::lock_guard _{mount_mutex};
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
        std::lock_guard _{mount_mutex};
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

    ssize_t read(int fd, void *buf, size_t size)
    {
        ifile *file = get_ifile(fd);
        if(file)
        {
            return file->read(buf, size);
        }
        errno = EBADF;
        return -1;
    }

    ssize_t write(int fd, const void *buf, size_t size)
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

    off_t lseek(int fd, off_t offset, int whence)
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

    // POSIX 目录接口
    DIR *opendir(const char *pathname)
    {
        int fd = open(pathname, O_RDONLY | O_DIRECTORY);
        if(fd < 0)
            return nullptr;

        DIR *d = new(std::nothrow) DIR{};
        if(d == nullptr)
        {
            close(fd);
            errno = ENOMEM;
            return nullptr;
        }
        d->fd = fd;
        return d;
    }

    DIR *fdopendir(int fd)
    {
        struct stat buf;
        if(fstat(fd, &buf) < 0)
            return nullptr;
        if(!S_ISDIR(buf.st_mode))
        {
            errno = ENOTDIR;
            return nullptr;
        }

        DIR *d = new(std::nothrow) DIR{};
        if(d == nullptr)
        {
            errno = ENOMEM;
            return nullptr;
        }
        d->fd = fd;
        return d;
    }

    xeno::dirent *readdir(DIR *dirp)
    {
        if(dirp == nullptr)
        {
            errno = EBADF;
            return nullptr;
        }

        ifile *file = get_ifile(dirp->fd);
        if(file == nullptr)
        {
            errno = EBADF;
            return nullptr;
        }

        if(file->readdir(dirp->entry) != 1)
            return nullptr; // 0=EOF 保持 errno；-1 由文件系统设置

        return &dirp->entry;
    }

    int readdir_r(DIR *dirp, xeno::dirent *entry, xeno::dirent **result)
    {
        if(dirp == nullptr || entry == nullptr || result == nullptr)
            return EINVAL;

        errno = 0;
        xeno::dirent *r = readdir(dirp);
        if(r == nullptr)
        {
            int e = errno;
            *result = nullptr;
            return e;
        }

        *entry = *r;
        *result = entry;
        return 0;
    }

    int closedir(DIR *dirp)
    {
        if(dirp == nullptr)
        {
            errno = EBADF;
            return -1;
        }

        int r = close(dirp->fd);
        delete dirp;
        return r;
    }

    void rewinddir(DIR *dirp)
    {
        if(dirp != nullptr)
            lseek(dirp->fd, 0, SEEK_SET);
    }

    void seekdir(DIR *dirp, long loc)
    {
        if(dirp != nullptr)
            lseek(dirp->fd, loc, SEEK_SET);
    }

    long telldir(DIR *dirp)
    {
        if(dirp == nullptr)
        {
            errno = EBADF;
            return -1;
        }
        return (long)lseek(dirp->fd, 0, SEEK_CUR);
    }

    int dirfd(DIR *dirp)
    {
        if(dirp == nullptr)
        {
            errno = EBADF;
            return -1;
        }
        return dirp->fd;
    }

    int mkdir(const char *pathname, mode_t mode)
    {
        if(pathname == nullptr)
        {
            errno = EFAULT;
            return -1;
        }

        path p{pathname};
        {
            std::lock_guard _{mount_mutex};
            if(mountpoints.contains(p))
            {
                errno = EEXIST;
                return -1;
            }
        }

        auto [fs, relative] = get_ifs_and_prefix(p);
        if(fs == nullptr)
        {
            errno = ENOENT;
            return -1;
        }
        return fs->mkdir(relative.c_str(), mode);
    }

    int rmdir(const char *pathname)
    {
        if(pathname == nullptr)
        {
            errno = EFAULT;
            return -1;
        }

        path p{pathname};
        {
            std::lock_guard _{mount_mutex};
            if(mountpoints.contains(p))
            {
                errno = EBUSY;
                return -1;
            }
        }

        auto [fs, relative] = get_ifs_and_prefix(p);
        if(fs == nullptr)
        {
            errno = ENOENT;
            return -1;
        }
        return fs->rmdir(relative.c_str());
    }

    int unlink(const char *pathname)
    {
        if(pathname == nullptr)
        {
            errno = EFAULT;
            return -1;
        }

        path p{pathname};
        {
            std::lock_guard _{mount_mutex};
            if(mountpoints.contains(p))
            {
                errno = EBUSY;
                return -1;
            }
        }

        auto [fs, relative] = get_ifs_and_prefix(p);
        if(fs == nullptr)
        {
            errno = ENOENT;
            return -1;
        }
        return fs->unlink(relative.c_str());
    }

    int rename(const char *oldpath, const char *newpath)
    {
        if(oldpath == nullptr || newpath == nullptr)
        {
            errno = EFAULT;
            return -1;
        }

        path op{oldpath};
        path np{newpath};
        {
            std::lock_guard _{mount_mutex};
            if(mountpoints.contains(op) || mountpoints.contains(np))
            {
                errno = EBUSY;
                return -1;
            }
        }

        auto [ofs, orel] = get_ifs_and_prefix(op);
        auto [nfs, nrel] = get_ifs_and_prefix(np);
        if(ofs == nullptr || nfs == nullptr)
        {
            errno = ENOENT;
            return -1;
        }
        if(ofs != nfs)
        {
            errno = EXDEV;
            return -1;
        }
        return ofs->rename(orel.c_str(), nrel.c_str());
    }

    int stat(const char *pathname, struct stat *buf)
    {
        if(pathname == nullptr || buf == nullptr)
        {
            errno = EFAULT;
            return -1;
        }

        path p{pathname};
        auto [fs, relative] = get_ifs_and_prefix(p);
        if(fs != nullptr)
        {
            if(fs->stat(relative.c_str(), buf) == 0)
                return 0;

            int saved = errno;
            // 挂载点本身必须表现为目录，即使被挂载的文件系统不支持 stat
            std::lock_guard _{mount_mutex};
            if(mountpoints.contains(p))
            {
                *buf = {};
                buf->st_mode = S_IFDIR | 0555;
                buf->st_nlink = 2;
                return 0;
            }
            errno = saved;
            return -1;
        }

        errno = ENOENT;
        return -1;
    }

};

inline int ifs::stat(const char *path, struct stat *buf)
{
    int fd = open(path, O_RDONLY);
    if(fd < 0)
        return -1;

    ifile *file = this->vfs().fget(fd);
    int r = -1;
    if(file != nullptr)
        r = file->fstat(buf);
    else
        errno = EBADF;

    close(fd);
    return r;
}

}
