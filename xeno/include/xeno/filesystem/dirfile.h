#pragma once

#include<algorithm>
#include<cstring>
#include<string>
#include<utility>
#include<vector>

#include"vfs.h"

namespace xeno
{

// 目录句柄基类：open 时对目录项做快照，readdir/lseek 只操作自己的游标。
class dirfile: public ifile
{
protected:
    struct entry
    {
        std::string name;
        mode_t mode;
        ino_t ino;
    };

    int fd = -1;
    mode_t dir_mode = S_IFDIR | 0555;
    ino_t dir_ino = 0;
    std::vector<entry> entries; // open 时快照，之后只读
    size_t index = 0;

public:
    dirfile(xeno::ifs *fs, mode_t mode, ino_t ino)
        : ifile{fs}, dir_mode{mode}, dir_ino{ino} {}

    void add_entry(std::string name, mode_t mode, ino_t ino)
    {
        entries.push_back({std::move(name), mode, ino});
    }

    int open(const char *, int) override
    {
        fd = fd_alloc();
        return fd;
    }

    int close() override
    {
        if(fd == -1)
            return 0;
        int f = fd;
        fd = -1;
        return fd_dealloc(f);
    }

    int readdir(xeno::dirent &e) override
    {
        if(index >= entries.size())
            return 0;

        auto &x = entries[index++];
        e.d_ino = x.ino;
        e.d_type = IFTODT(x.mode);

        size_t n = std::min(x.name.size(), sizeof(e.d_name) - 1);
        std::memcpy(e.d_name, x.name.data(), n);
        e.d_name[n] = '\0';
        return 1;
    }

    off_t lseek(off_t offset, int whence) override
    {
        off_t base;
        switch(whence)
        {
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = (off_t)index; break;
        case SEEK_END: base = (off_t)entries.size(); break;
        default:
            errno = EINVAL;
            return -1;
        }

        off_t n = base + offset;
        if(n < 0)
        {
            errno = EINVAL;
            return -1;
        }
        index = (size_t)n;
        return n;
    }

    int read(void *, size_t) override
    {
        errno = EISDIR;
        return -1;
    }

    int write(const void *, size_t) override
    {
        errno = EISDIR;
        return -1;
    }

    int truncate(const char *, off_t) override
    {
        errno = EISDIR;
        return -1;
    }

    int fstat(struct stat *buf) override
    {
        *buf = {};
        buf->st_ino = dir_ino;
        buf->st_mode = dir_mode;
        buf->st_nlink = 2;
        buf->st_size = 0;
        buf->st_blksize = 512;
        buf->st_blocks = 0;
        return 0;
    }
};

}
