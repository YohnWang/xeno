/* newlib 的 <dirent.h> 会包含本文件；工具链自带的同名文件只有 #error。 */

#ifndef _SYS_DIRENT_H_
#define _SYS_DIRENT_H_

#include <sys/cdefs.h>
#include <sys/types.h>
#include <sys/syslimits.h>

__BEGIN_DECLS

struct dirent
{
    ino_t d_ino;
    unsigned char d_type;
    char d_name[NAME_MAX + 1];
};

#define d_fileno d_ino

/* xeno 的目录句柄。POSIX 只要求 DIR 是不透明类型；这里是 libc 端口，
   struct 定义公开，但应用仍应只通过 DIR* 使用。 */
typedef struct __dirstream
{
    int fd;                  /* xeno VFS fd */
    struct dirent entry;     /* readdir 返回缓冲 */
} DIR;

#ifndef MAXNAMLEN
#define MAXNAMLEN NAME_MAX
#endif

#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK    10
#define DT_SOCK   12
#define DT_WHT    14

#define IFTODT(mode)    (((mode) & 0170000) >> 12)
#define DTTOIF(type)    ((type) << 12)

#define _DIRENT_HAVE_D_TYPE 1

__END_DECLS

#endif
