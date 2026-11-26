#include<xeno/filesystem.h>
#include<dirent.h>
#include<reent.h>
#include<unistd.h>



extern "C"
int _isatty(int fd)
{
    if(fd == STDIN_FILENO || fd == STDOUT_FILENO || fd == STDERR_FILENO)
        return 1;
    return 0;
}

int _open_r(struct _reent *ptr, const char *file, int flags, int mode)
{
    return __vfs.open(file, flags);
}

int _close_r(struct _reent *ptr, int fd)
{
    return __vfs.close(fd);
}

_ssize_t _read_r(struct _reent *ptr, int fd, void *buf, size_t cnt)
{
    return __vfs.read(fd, buf, cnt);
}

_ssize_t _write_r(struct _reent *ptr, int fd, const void *buf, size_t cnt)
{
    return __vfs.write(fd, buf, cnt);
}

_off_t _lseek_r(struct _reent *ptr, int fd, _off_t offset, int whence)
{
    return __vfs.lseek(fd, offset, whence);
}

int _fstat_r(struct _reent *ptr, int fd, struct stat *st)
{
    return __vfs.fstat(fd, st);
}

int _stat_r(struct _reent *ptr, const char * pathname, struct stat * statbuf)
{
    return __vfs.stat(pathname, statbuf);
}

extern "C" DIR *opendir(const char *name)
{
    return __vfs.opendir(name);
}

extern "C" DIR *fdopendir(int fd)
{
    return __vfs.fdopendir(fd);
}

extern "C" struct dirent *readdir(DIR *dirp)
{
    return __vfs.readdir(dirp);
}

extern "C" int readdir_r(DIR *dirp, struct dirent *entry, struct dirent **result)
{
    return __vfs.readdir_r(dirp, entry, result);
}

extern "C" int closedir(DIR *dirp)
{
    return __vfs.closedir(dirp);
}

extern "C" void rewinddir(DIR *dirp)
{
    __vfs.rewinddir(dirp);
}

extern "C" void seekdir(DIR *dirp, long loc)
{
    __vfs.seekdir(dirp, loc);
}

extern "C" long telldir(DIR *dirp)
{
    return __vfs.telldir(dirp);
}

extern "C" int dirfd(DIR *dirp)
{
    return __vfs.dirfd(dirp);
}