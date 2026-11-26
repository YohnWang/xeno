#pragma once

#include<string>
#include<string_view>
#include<flat_map>
#include<mutex>

#include"vfs.h"
#include"dirfile.h"
#include"path.h"

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

    ~devfile() override
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

    int fstat(struct stat *buf) override
    {
        std::lock_guard _{m};
        *buf = {};
        buf->st_mode = S_IFCHR | 0666;
        buf->st_nlink = 1;
        buf->st_blksize = 512;
        return 0;
    }
};

class devfs: public ifs
{
    friend class devfile;

    std::flat_map<path, devfile*> devfiles;
    std::flat_map<int, ifile*> dirs; // 已打开的目录 fd，关闭时需要 delete
    std::recursive_mutex mutex;
    ino_t next_ino = 2;

    ino_t alloc_ino_locked()
    {
        return next_ino++;
    }

    bool is_dir_locked(const xeno::path &p) const
    {
        if(p.is_root())
            return true;

        std::string_view ps = p;
        for(auto &&[k, f]: devfiles)
        {
            std::string_view ks = k;
            if(ks.size() > ps.size() && ks.starts_with(ps) && ks[ps.size()] == '/')
                return true;
        }
        return false;
    }

public:

    int mknod(path p, devfile *file)
    {
        std::lock_guard _{mutex};
        if(p.filename().size() > NAME_MAX)
        {
            errno = ENAMETOOLONG;
            return -1;
        }
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
        xeno::path p = path;

        if(auto it = devfiles.find(p); it != devfiles.end())
        {
            if((mode & O_CREAT) && (mode & O_EXCL))
            {
                errno = EEXIST;
                return -1;
            }
            if(mode & O_DIRECTORY)
            {
                errno = ENOTDIR;
                return -1;
            }
            return it->second->open(path, mode);
        }

        if(!is_dir_locked(p))
        {
            errno = ENOENT;
            return -1;
        }

        if((mode & O_CREAT) && (mode & O_EXCL))
        {
            errno = EEXIST;
            return -1;
        }
        if((mode & O_ACCMODE) != O_RDONLY || (mode & O_TRUNC))
        {
            errno = EISDIR;
            return -1;
        }

        const mode_t dir_mode = S_IFDIR | 0555;
        const ino_t dir_ino = alloc_ino_locked();
        auto *file = new dirfile{this, dir_mode, dir_ino};

        file->add_entry(".", dir_mode, dir_ino);
        file->add_entry("..", S_IFDIR | 0555, 1);
        for(auto &&[k, f]: devfiles)
        {
            if(k.parent_path() == p)
                file->add_entry(std::string{k.filename()}, S_IFCHR | 0666, alloc_ino_locked());
        }

        int fd = file->open(path, mode);
        if(fd < 0)
        {
            delete file;
            return -1;
        }
        dirs.emplace(fd, file);
        return fd;
    }

    int close(int fd) override
    {
        std::lock_guard _{mutex};
        ifile *f = vfs().fget(fd);
        if(f == nullptr)
        {
            errno = EBADF;
            return -1;
        }

        bool directory = dirs.erase(fd) != 0;
        int r = f->close();
        if(directory)
            delete f;
        return r;
    }

    int stat(const char *pathname, struct stat *buf) override
    {
        std::lock_guard _{mutex};
        xeno::path p{pathname};

        if(devfiles.contains(p))
        {
            *buf = {};
            buf->st_mode = S_IFCHR | 0666;
            buf->st_nlink = 1;
            buf->st_blksize = 512;
            return 0;
        }

        if(is_dir_locked(p))
        {
            *buf = {};
            buf->st_mode = S_IFDIR | 0555;
            buf->st_nlink = 2;
            return 0;
        }

        errno = ENOENT;
        return -1;
    }
};

}
