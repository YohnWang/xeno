#include<xeno/filesystem.h>
#include<reent.h>
#include<unistd.h>



extern "C"
int _isatty(struct _reent *, int fd)
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