#ifndef __LOG_SEMIHOST_H__
#define __LOG_SEMIHOST_H__

#include <stdint.h>

// Semihosting syscall numbers (ARM 半主机系统调用编号)
#define SYS_WRITE0     0x04  // 将空终止字符串输出到 stdout（控制台）
#define SYS_READC      0x07  // 从 stdin 读取单个字符（无需文件句柄，阻塞等待）
#define SYS_READ       0x06  // 从已打开的文件描述符中读取指定长度数据
#define SYS_CLOCK      0x10  // 返回自程序执行开始以来的毫秒数（计满约49天后回绕）
#define SYS_CLOSE      0x02  // 关闭已打开的文件句柄，释放资源
#define SYS_ELAPSED    0x30  // 返回目标机运行以来的滴答数及当前滴答频率（用于高精度时间测量）
#define SYS_ERRNO      0x13  // 返回最后一次失败的半主机系统调用的错误码
#define SYS_EXIT       0x18  // 终止程序执行（不返回应用程序退出码，由调试器接管）
#define SYS_EXIT_EXTENDED 0x20 // 终止程序并返回指定的应用程序退出码（支持 0x200000+ 状态）
#define SYS_FLEN       0x0C  // 获取已打开文件的长度（以字节为单位）
#define SYS_GET_CMDLINE 0x15 // 获取宿主机的原始命令行参数字符串（含程序名）
#define SYS_HEAPINFO   0x16  // 获取堆（Heap）的起始地址和终止地址信息（用于 C 库初始化）
#define SYS_ISERROR    0x08  // 检查返回码是否为半主机错误状态（ARM 规范中通常检查最高位是否置位）
#define SYS_ISTTY      0x09  // 检查给定的文件句柄是否关联到字符终端（TTY / 控制台）
#define SYS_OPEN       0x01  // 打开宿主机上的文件（需传入路径和模式，返回文件句柄）
#define SYS_REMOVE     0x0E  // 删除宿主机上指定路径的文件（unlink）
#define SYS_RENAME     0x0F  // 重命名或移动宿主机上的指定文件
#define SYS_SEEK       0x0A  // 移动已打开文件的读写指针（基于起始位置进行偏移）
#define SYS_SYSTEM     0x12  // 调用宿主机的系统 Shell 执行命令行字符串
#define SYS_TICKFREQ   0x31  // 返回目标机的滴答定时器频率（用于将 SYS_ELAPSED 换算为秒）
#define SYS_TIME       0x11  // 返回自 1970-01-01 00:00:00 UTC 以来的秒数（Unix 时间戳）
#define SYS_TMPNAM     0x0D  // 在宿主机文件系统上生成一个可用的临时文件名
#define SYS_WRITE      0x05  // 向已打开的文件描述符写入指定长度的数据（需传入缓冲区）
#define SYS_WRITEC     0x03  // 将一个字符写入半主机调试终端（通常用于输出单个调试字符）

// Semihosting Magic Numbers (半主机魔数，用于标识特定的结构体或文件传输块)
#define SHFB_MAGIC_0   0x53  // 'S' - 半主机文件块（Semihosting File Block）魔数第 0 字节
#define SHFB_MAGIC_1   0x48  // 'H' - 魔数第 1 字节
#define SHFB_MAGIC_2   0x46  // 'F' - 魔数第 2 字节
#define SHFB_MAGIC_3   0x42  // 'B' - 魔数第 3 字节（完整组合为 ASCII 码 "SHFB"）

#ifdef __cplusplus
extern "C" {
#endif

uintptr_t sys_semihost(uintptr_t syscall, const void *params);

#ifdef __cplusplus
}
#endif

#endif /* __LOG_SEMIHOST_H__ */
