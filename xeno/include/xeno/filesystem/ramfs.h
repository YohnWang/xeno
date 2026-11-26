#pragma once

#include<algorithm>
#include<cstring>
#include<flat_map>
#include<functional>
#include<mutex>
#include<string>
#include<string_view>
#include<utility>
#include<vector>

#include"vfs.h"
#include"dirfile.h"
#include"path.h"

namespace xeno
{

struct ramnode
{
    mode_t mode = 0;      // S_IFDIR|perm 或 S_IFREG|perm
    ino_t ino = 0;
    unsigned refs = 0;    // 打开的句柄数
    bool linked = true;   // 是否还在父目录 children 里
    ramnode *parent = nullptr;
    std::string data;     // 普通文件内容

    // 目录项；std::less<> 允许用 string_view 直接查找
    std::flat_map<std::string, ramnode*, std::less<>> children;
};

class ramfs;

// 每次 open 新建一个普通文件句柄
class ramfile final: public ifile
{
    friend class ramfs;

    ramfs *fs;
    ramnode *node;
    int flags;
    int fd = -1;
    off_t pos = 0;

    ramfile(ramfs *f, ramnode *n, int fl);

public:
    ~ramfile() override;

    int open(const char *, int mode) override
    {
        flags = mode;
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

    int read(void *buf, size_t size) override;
    int write(const void *buf, size_t size) override;
    off_t lseek(off_t offset, int whence) override;
    int truncate(const char *, off_t length) override;
    int fstat(struct stat *buf) override;
};

// 每次 open 新建一个目录句柄，遍历游标是每个 fd 独立的
class ramdir final: public dirfile
{
    friend class ramfs;

    ramfs *fs;
    ramnode *node;

    ramdir(ramfs *f, ramnode *n, mode_t mode, ino_t ino);

public:
    ~ramdir() override;
};

// 内存文件系统：目录树 + 普通文件
class ramfs final: public ifs
{
    friend class ramfile;
    friend class ramdir;

    ramnode *root = nullptr;
    std::recursive_mutex mutex;
    ino_t next_ino = 0;

    ino_t alloc_ino_locked()
    {
        if(++next_ino == 0)
            ++next_ino;
        return next_ino;
    }

    ramnode *lookup_locked(const xeno::path &p) const
    {
        if(p.is_root())
            return root;

        ramnode *n = root;
        std::string_view rest = p;
        while(!rest.empty())
        {
            auto slash = rest.find('/', 1);
            std::string_view name = rest.substr(1,
                slash == std::string_view::npos ? std::string_view::npos : slash - 1);
            if(name.empty())
                break;
            if(!S_ISDIR(n->mode))
                return nullptr;

            auto it = n->children.find(name);
            if(it == n->children.end())
                return nullptr;
            n = it->second;

            if(slash == std::string_view::npos)
                break;
            rest.remove_prefix(slash);
        }
        return n;
    }

    ramnode *lookup_parent_locked(const xeno::path &p, std::string_view &name) const
    {
        name = p.filename();
        return lookup_locked(p.parent_path());
    }

    void release(ramnode *n)
    {
        std::lock_guard _{mutex};
        if(--n->refs == 0 && !n->linked)
            delete n;
    }

    int open_node_locked(ramnode *n, int flags)
    {
        if(S_ISDIR(n->mode))
        {
            auto *f = new ramdir{this, n, n->mode, n->ino};
            f->add_entry(".", n->mode, n->ino);

            ramnode *parent = n->parent != nullptr ? n->parent : n;
            f->add_entry("..", parent->mode, parent->ino);
            for(auto &&[name, child]: n->children)
                f->add_entry(name, child->mode, child->ino);

            int fd = f->open("", flags);
            if(fd < 0)
            {
                delete f;
                return -1;
            }
            return fd;
        }

        auto *f = new ramfile{this, n, flags};
        int fd = f->open("", flags);
        if(fd < 0)
        {
            delete f;
            return -1;
        }
        return fd;
    }

public:
    ramfs()
    {
        root = new ramnode;
        root->mode = S_IFDIR | 0755;
        root->ino = alloc_ino_locked();
        root->parent = root;
    }

    int open(const char *pathname, int flags) override
    {
        std::lock_guard _{mutex};
        xeno::path p{pathname};
        int acc = flags & O_ACCMODE;

        ramnode *n = lookup_locked(p);
        if(n != nullptr)
        {
            if((flags & O_CREAT) && (flags & O_EXCL))
            {
                errno = EEXIST;
                return -1;
            }

            if(S_ISDIR(n->mode))
            {
                if(acc != O_RDONLY || (flags & O_TRUNC))
                {
                    errno = EISDIR;
                    return -1;
                }
            }
            else
            {
                if(flags & O_DIRECTORY)
                {
                    errno = ENOTDIR;
                    return -1;
                }
                if((flags & O_TRUNC) && acc != O_RDONLY)
                    n->data.clear();
            }
            return open_node_locked(n, flags);
        }

        if(!(flags & O_CREAT))
        {
            errno = ENOENT;
            return -1;
        }
        if(flags & O_DIRECTORY)
        {
            errno = EINVAL;
            return -1;
        }

        std::string_view name;
        ramnode *parent = lookup_parent_locked(p, name);
        if(parent == nullptr)
        {
            errno = ENOENT;
            return -1;
        }
        if(!S_ISDIR(parent->mode))
        {
            errno = ENOTDIR;
            return -1;
        }
        if(name.size() > NAME_MAX)
        {
            errno = ENAMETOOLONG;
            return -1;
        }

        auto *created = new ramnode;
        created->mode = S_IFREG | 0644;
        created->ino = alloc_ino_locked();
        created->parent = parent;
        parent->children.emplace(std::string{name}, created);
        return open_node_locked(created, flags);
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

        int r = f->close();
        delete f;
        return r;
    }

    int mkdir(const char *pathname, mode_t mode) override
    {
        std::lock_guard _{mutex};
        xeno::path p{pathname};
        if(p.is_root() || lookup_locked(p) != nullptr)
        {
            errno = EEXIST;
            return -1;
        }

        std::string_view name;
        ramnode *parent = lookup_parent_locked(p, name);
        if(parent == nullptr)
        {
            errno = ENOENT;
            return -1;
        }
        if(!S_ISDIR(parent->mode))
        {
            errno = ENOTDIR;
            return -1;
        }
        if(name.size() > NAME_MAX)
        {
            errno = ENAMETOOLONG;
            return -1;
        }

        auto *created = new ramnode;
        created->mode = S_IFDIR | (mode & 07777);
        created->ino = alloc_ino_locked();
        created->parent = parent;
        parent->children.emplace(std::string{name}, created);
        return 0;
    }

    int rmdir(const char *pathname) override
    {
        std::lock_guard _{mutex};
        xeno::path p{pathname};
        if(p.is_root())
        {
            errno = EBUSY;
            return -1;
        }

        std::string_view name;
        ramnode *parent = lookup_parent_locked(p, name);
        if(parent == nullptr)
        {
            errno = ENOENT;
            return -1;
        }

        auto it = parent->children.find(name);
        if(it == parent->children.end())
        {
            errno = ENOENT;
            return -1;
        }

        ramnode *n = it->second;
        if(!S_ISDIR(n->mode))
        {
            errno = ENOTDIR;
            return -1;
        }
        if(!n->children.empty())
        {
            errno = ENOTEMPTY;
            return -1;
        }

        parent->children.erase(std::string{name});
        n->linked = false;
        if(n->refs == 0)
            delete n;
        return 0;
    }

    int unlink(const char *pathname) override
    {
        std::lock_guard _{mutex};
        xeno::path p{pathname};
        if(p.is_root())
        {
            errno = EISDIR;
            return -1;
        }

        std::string_view name;
        ramnode *parent = lookup_parent_locked(p, name);
        if(parent == nullptr)
        {
            errno = ENOENT;
            return -1;
        }

        auto it = parent->children.find(name);
        if(it == parent->children.end())
        {
            errno = ENOENT;
            return -1;
        }

        ramnode *n = it->second;
        if(S_ISDIR(n->mode))
        {
            errno = EISDIR;
            return -1;
        }

        parent->children.erase(std::string{name});
        n->linked = false;
        if(n->refs == 0)
            delete n;
        return 0;
    }

    int rename(const char *oldpath, const char *newpath) override
    {
        std::lock_guard _{mutex};
        xeno::path op{oldpath};
        xeno::path np{newpath};

        if(op.is_root())
        {
            errno = EBUSY;
            return -1;
        }

        std::string_view oname;
        ramnode *oparent = lookup_parent_locked(op, oname);
        if(oparent == nullptr)
        {
            errno = ENOENT;
            return -1;
        }

        auto oit = oparent->children.find(oname);
        if(oit == oparent->children.end())
        {
            errno = ENOENT;
            return -1;
        }
        ramnode *src = oit->second;

        if(op == np)
            return 0;

        std::string_view nname;
        ramnode *nparent = lookup_parent_locked(np, nname);
        if(nparent == nullptr)
        {
            errno = ENOENT;
            return -1;
        }
        if(!S_ISDIR(nparent->mode))
        {
            errno = ENOTDIR;
            return -1;
        }
        if(nname.size() > NAME_MAX)
        {
            errno = ENAMETOOLONG;
            return -1;
        }

        // 不能把目录移进自己的子树
        for(ramnode *p = nparent; p != nullptr;)
        {
            if(p == src)
            {
                errno = EINVAL;
                return -1;
            }
            p = (p == root) ? nullptr : p->parent;
        }

        ramnode *dst = nullptr;
        if(auto nit = nparent->children.find(nname); nit != nparent->children.end())
            dst = nit->second;

        if(dst != nullptr && dst != src)
        {
            bool sd = S_ISDIR(src->mode);
            bool dd = S_ISDIR(dst->mode);
            if(sd != dd)
            {
                errno = sd ? ENOTDIR : EISDIR;
                return -1;
            }
            if(dd && !dst->children.empty())
            {
                errno = ENOTEMPTY;
                return -1;
            }
        }

        oparent->children.erase(std::string{oname});

        if(dst != nullptr && dst != src)
        {
            nparent->children.erase(std::string{nname});
            dst->linked = false;
            if(dst->refs == 0)
                delete dst;
        }

        src->parent = nparent;
        nparent->children.emplace(std::string{nname}, src);
        return 0;
    }

    int stat(const char *pathname, struct stat *buf) override
    {
        std::lock_guard _{mutex};
        xeno::path p{pathname};
        ramnode *n = lookup_locked(p);
        if(n == nullptr)
        {
            errno = ENOENT;
            return -1;
        }

        *buf = {};
        buf->st_ino = n->ino;
        buf->st_mode = n->mode;
        buf->st_nlink = S_ISDIR(n->mode) ? 2 : 1;
        buf->st_size = S_ISDIR(n->mode) ? 0 : (off_t)n->data.size();
        buf->st_blksize = 512;
        buf->st_blocks = (buf->st_size + 511) / 512;
        return 0;
    }
};

inline ramfile::ramfile(ramfs *f, ramnode *n, int fl):
    ifile{f}, fs{f}, node{n}, flags{fl}
{
    node->refs++;
}

inline ramdir::ramdir(ramfs *f, ramnode *n, mode_t mode, ino_t ino):
    dirfile{f, mode, ino}, fs{f}, node{n}
{
    node->refs++;
}

inline ramfile::~ramfile()
{
    fs->release(node);
}

inline int ramfile::read(void *buf, size_t size)
{
    std::lock_guard _{fs->mutex};
    if((flags & O_ACCMODE) == O_WRONLY)
    {
        errno = EBADF;
        return -1;
    }
    if((size_t)pos >= node->data.size())
        return 0;

    size_t n = std::min(size, node->data.size() - (size_t)pos);
    std::memcpy(buf, node->data.data() + pos, n);
    pos += (off_t)n;
    return (int)n;
}

inline int ramfile::write(const void *buf, size_t size)
{
    std::lock_guard _{fs->mutex};
    if((flags & O_ACCMODE) == O_RDONLY)
    {
        errno = EBADF;
        return -1;
    }
    if(size == 0)
        return 0;

    if(flags & O_APPEND)
        pos = (off_t)node->data.size();
    if(pos > (off_t)node->data.size())
        node->data.resize((size_t)pos, '\0');

    node->data.resize((size_t)pos + size);
    std::memcpy(node->data.data() + pos, buf, size);
    pos += (off_t)size;
    return (int)size;
}

inline off_t ramfile::lseek(off_t offset, int whence)
{
    std::lock_guard _{fs->mutex};
    off_t base;
    switch(whence)
    {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = pos; break;
    case SEEK_END: base = (off_t)node->data.size(); break;
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
    pos = n;
    return n;
}

inline int ramfile::truncate(const char *, off_t length)
{
    std::lock_guard _{fs->mutex};
    if(length < 0)
    {
        errno = EINVAL;
        return -1;
    }
    node->data.resize((size_t)length);
    return 0;
}

inline int ramfile::fstat(struct stat *buf)
{
    std::lock_guard _{fs->mutex};
    *buf = {};
    buf->st_ino = node->ino;
    buf->st_mode = node->mode;
    buf->st_nlink = 1;
    buf->st_size = (off_t)node->data.size();
    buf->st_blksize = 512;
    buf->st_blocks = (buf->st_size + 511) / 512;
    return 0;
}

inline ramdir::~ramdir()
{
    fs->release(node);
}

}
