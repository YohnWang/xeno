#pragma once
#include<stdlib.h>
#include<stddef.h>
#include<time.h>
#include"port.h"
#include<stdio.h>

#define __xeno_macro_tostr_(...) #__VA_ARGS__
#define __xeno_macro_tostr(x) __xeno_macro_tostr_(x)

#define __xeno_macro_cat_(a, b) a##b
#define __xeno_macro_cat(a, b) __xeno_macro_cat_(a, b)

#define xeno_link_wrap(x) __xeno_macro_cat(__wrap_, x)

#define __assert_failed(cond) \
if(!(cond)){__write_terminal("assertion failed in: ");\
    __write_terminal(__FILE__);__write_terminal(":");__write_terminal(__xeno_macro_tostr(__LINE__));\
    __write_terminal("\r\n");__write_terminal("  ");\
    __write_terminal(__xeno_macro_tostr(cond));__write_terminal("\r\n");abort();}

#ifdef __cplusplus

#include<charconv>
#include<array>
#include<atomic>
#include<chrono>
#include<condition_variable>
#include<cstring>
#include<errno.h>
#include<limits.h>
#include<mutex>

#include<bits/futex.h>
#include<xeno/critical.h>

namespace xeno
{

template <typename T, size_t N>
class static_queue
{
    static constexpr size_t queue_size = N;
    std::array<T, queue_size> a{};
    size_t front = 0;
    size_t rear = 0;

public:
    // 入队，满则返回 false
    bool push(const T &v)
    {
        if (rear - front >= queue_size)
            return false;
        a[rear % N] = v;
        ++rear;
        return true;
    }

    // 入队，若满则覆盖最旧元素（队列始终保持满状态）
    void push_overwrite(const T &v)
    {
        if (rear - front >= queue_size)
        {
            ++front; // 丢弃最旧元素
        }
        a[rear % N] = v;
        ++rear;
    }

    // 出队，空则返回 false
    bool pop(T &v)
    {
        if (front == rear)
            return false;
        v = a[front % N];
        ++front;
        return true;
    }

    bool is_empty() const
    {
        return rear == front;
    }

    bool is_full() const
    {
        return rear - front >= queue_size;
    }

    size_t size() const
    {
        return rear - front;
    }

    // 按入队顺序访问，0 为最旧的元素
    T &operator[](size_t i)
    {
        return a[(front + i) % N];
    }

    const T &operator[](size_t i) const
    {
        return a[(front + i) % N];
    }
};

// fifo缓冲：单读线程 + 单写线程（由使用者保证，不支持单线程环境）
//
// 整笔语义 + 分块推进：read/write 以 n 字节为目标，内部按当前可用的数据/空间分块拷贝，
// 块与块之间等待并让出缓冲区，因此 n 可以大于 N，读写两端可以边写边读。
// notify_reader()/notify_writer() 是一次性提示：让对端停止等待，返回已处理字节数
// （可能为 0）；提示被消费后恢复正常。
// finish_write()/finish_read() 是永久结束：读端读空缓冲后得到 EOF（read 返回 0），
// 对已结束方向的读写返回 -1/EPIPE；write_finished()/read_finished() 可无锁查询。
// clear() 只清空缓冲数据；reset() 完整复位（调用时两端必须空闲），可重新复用对象。
// 出错返回 -1，read_errno()/write_errno() 给出 errno.h 错误码（成功不清零）。
template <size_t N>
class fifo_buffer
{
    static_assert(N > 0, "fifo_buffer: N must be positive");
    static_assert(N <= static_cast<size_t>(INT_MAX), "fifo_buffer: N must fit int");

    std::array<unsigned char, N> buffer_;
    size_t read_pos_ = 0;           // 读游标，[0, N)
    size_t write_pos_ = 0;          // 写游标，[0, N)
    size_t used_ = 0;               // 已存字节数，[0, N]
    bool reader_notified_ = false;  // 待消费：提示读端提前退出
    bool writer_notified_ = false;  // 待消费：提示写端提前退出
    std::atomic<bool> write_finished_{false};  // 写端永久结束
    std::atomic<bool> read_finished_{false};   // 读端永久结束
    int read_errno_ = 0;
    int write_errno_ = 0;
    std::mutex mutex_;
    std::condition_variable data_cv_;   // 读端等待：write_finished_ || reader_notified_ || used_ != 0
    std::condition_variable space_cv_;  // 写端等待：writer_notified_ || read_finished_ || free_space() != 0

    size_t free_space() const
    {
        return N - used_;
    }

    int fail_read(int e)
    {
        read_errno_ = e;
        return -1;
    }

    int fail_write(int e)
    {
        write_errno_ = e;
        return -1;
    }

    // 以下要求持锁，0 <= n <= N；n == 0 直接返回；跨尾用两段 memcpy
    void copy_in(const void *buf, size_t n)
    {
        if (n == 0)
            return;
        auto p = static_cast<const unsigned char *>(buf);
        size_t first = n < N - write_pos_ ? n : N - write_pos_;
        std::memcpy(buffer_.data() + write_pos_, p, first);
        if (n > first)
            std::memcpy(buffer_.data(), p + first, n - first);
        write_pos_ += n;
        if (write_pos_ >= N)
            write_pos_ -= N;
        used_ += n;
    }

    void copy_out(void *buf, size_t n)
    {
        if (n == 0)
            return;
        auto p = static_cast<unsigned char *>(buf);
        size_t first = n < N - read_pos_ ? n : N - read_pos_;
        std::memcpy(p, buffer_.data() + read_pos_, first);
        if (n > first)
            std::memcpy(p + first, buffer_.data(), n - first);
        read_pos_ += n;
        if (read_pos_ >= N)
            read_pos_ -= N;
        used_ -= n;
    }

public:

    // 尝试写入：写当前能写的字节数，返回实际写入数；完全没空间时 -1/EAGAIN
    int try_write(const void *buf, size_t n)
    {
        if (n == 0)
            return 0;
        if (n > static_cast<size_t>(INT_MAX))
            return fail_write(EINVAL);

        std::lock_guard lock{mutex_};
        if (write_finished() || read_finished())
            return fail_write(EPIPE);
        if (writer_notified_)
        {
            writer_notified_ = false;
            size_t k = free_space() < n ? free_space() : n;
            copy_in(buf, k);
            if (k)
                data_cv_.notify_one();
            return static_cast<int>(k);
        }
        size_t k = free_space() < n ? free_space() : n;
        if (k == 0)
            return fail_write(EAGAIN);
        copy_in(buf, k);
        data_cv_.notify_one();
        return static_cast<int>(k);
    }

    // 尝试在指定时间段内写入：尽量写满 n；超时时返回已写入数，零进展才 -1/ETIMEDOUT
    template<typename Rep, typename Period>
    int write_for(const std::chrono::duration<Rep, Period> &d, const void *buf, size_t n)
    {
        if (n == 0)
            return 0;
        if (n > static_cast<size_t>(INT_MAX))
            return fail_write(EINVAL);

        auto p = static_cast<const unsigned char *>(buf);
        size_t done = 0;
        bool wake_reader = false;
        std::unique_lock lock{mutex_};
        if (write_finished() || read_finished())
            return fail_write(EPIPE);
        auto deadline = std::chrono::steady_clock::now() + d;
        while (done < n)
        {
            if (read_finished())
                break; // 本次调用期间读端结束：返回已写部分，不置 errno
            if (writer_notified_)
            {
                writer_notified_ = false;
                size_t k = free_space() < n - done ? free_space() : n - done;
                if (k)
                {
                    copy_in(p + done, k);
                    done += k;
                    wake_reader = true;
                }
                break;
            }
            if (free_space() == 0)
            {
                if (wake_reader)
                {
                    data_cv_.notify_one();
                    wake_reader = false;
                }
                if (!space_cv_.wait_until(lock, deadline, [this] {
                        return writer_notified_ || read_finished() || free_space() != 0;
                    }))
                {
                    if (done == 0)
                        return fail_write(ETIMEDOUT);
                    break;
                }
                continue;
            }
            size_t k = free_space() < n - done ? free_space() : n - done;
            copy_in(p + done, k);
            done += k;
            wake_reader = true;
        }
        lock.unlock();
        if (wake_reader)
            data_cv_.notify_one();
        return static_cast<int>(done);
    }

    // 阻塞式写入：分块写满 n，直到完成、被 notify_writer() 提示或被 finish_read() 结束
    int write(const void *buf, size_t n)
    {
        if (n == 0)
            return 0;
        if (n > static_cast<size_t>(INT_MAX))
            return fail_write(EINVAL);

        auto p = static_cast<const unsigned char *>(buf);
        size_t done = 0;
        bool wake_reader = false;
        std::unique_lock lock{mutex_};
        if (write_finished() || read_finished())
            return fail_write(EPIPE);
        while (done < n)
        {
            if (read_finished())
                break; // 本次调用期间读端结束：返回已写部分，不置 errno
            if (writer_notified_)
            {
                writer_notified_ = false;
                size_t k = free_space() < n - done ? free_space() : n - done;
                if (k)
                {
                    copy_in(p + done, k);
                    done += k;
                    wake_reader = true;
                }
                break;
            }
            if (free_space() == 0)
            {
                if (wake_reader)
                {
                    data_cv_.notify_one();
                    wake_reader = false;
                }
                space_cv_.wait(lock, [this] {
                    return writer_notified_ || read_finished() || free_space() != 0;
                });
                continue;
            }
            size_t k = free_space() < n - done ? free_space() : n - done;
            copy_in(p + done, k);
            done += k;
            wake_reader = true;
        }
        lock.unlock();
        if (wake_reader)
            data_cv_.notify_one();
        return static_cast<int>(done);
    }

    // 提示读端提前退出：当前/下一次 read 停止等待，返回已读字节数
    void notify_reader()
    {
        {
            std::lock_guard lock{mutex_};
            reader_notified_ = true;
        }
        data_cv_.notify_all();
    }

    // 永久结束写端：读端排空缓冲后得到 EOF（read 返回 0）；之后的写返回 -1/EPIPE
    void finish_write()
    {
        {
            std::lock_guard lock{mutex_};
            reader_notified_ = false;
            write_finished_.store(true, std::memory_order_relaxed);
        }
        data_cv_.notify_all();
    }

    // 写端是否已永久结束；无锁查询
    bool write_finished() const noexcept
    {
        return write_finished_.load(std::memory_order_relaxed);
    }

    // 尝试读取：读当前能读的字节数；空缓冲的 0 可能是 EOF，-1/EAGAIN 表示暂时没数据
    int try_read(void *buf, size_t n)
    {
        if (n == 0)
            return 0;
        if (n > static_cast<size_t>(INT_MAX))
            return fail_read(EINVAL);

        std::lock_guard lock{mutex_};
        if (read_finished())
            return fail_read(EPIPE);
        if (write_finished() && used_ == 0)
            return 0; // EOF
        if (reader_notified_)
        {
            reader_notified_ = false;
            size_t k = used_ < n ? used_ : n;
            copy_out(buf, k);
            if (k)
                space_cv_.notify_one();
            return static_cast<int>(k);
        }
        size_t k = used_ < n ? used_ : n;
        if (k == 0)
            return fail_read(EAGAIN);
        copy_out(buf, k);
        space_cv_.notify_one();
        return static_cast<int>(k);
    }

    // 指定时间内尝试读取：尽量读满 n；超时时返回已读出数，零进展才 -1/ETIMEDOUT
    template<typename Rep, typename Period>
    int read_for(const std::chrono::duration<Rep, Period> &d, void *buf, size_t n)
    {
        if (n == 0)
            return 0;
        if (n > static_cast<size_t>(INT_MAX))
            return fail_read(EINVAL);

        auto p = static_cast<unsigned char *>(buf);
        size_t done = 0;
        bool wake_writer = false;
        std::unique_lock lock{mutex_};
        if (read_finished())
            return fail_read(EPIPE);
        auto deadline = std::chrono::steady_clock::now() + d;
        while (done < n)
        {
            if (write_finished() && used_ == 0)
                break; // 排空后 EOF，done 可为 0
            if (reader_notified_)
            {
                reader_notified_ = false;
                size_t k = used_ < n - done ? used_ : n - done;
                if (k)
                {
                    copy_out(p + done, k);
                    done += k;
                    wake_writer = true;
                }
                break;
            }
            if (used_ == 0)
            {
                if (wake_writer)
                {
                    space_cv_.notify_one();
                    wake_writer = false;
                }
                if (!data_cv_.wait_until(lock, deadline, [this] {
                        return write_finished() || reader_notified_ || used_ != 0;
                    }))
                {
                    if (done == 0)
                        return fail_read(ETIMEDOUT);
                    break;
                }
                continue;
            }
            size_t k = used_ < n - done ? used_ : n - done;
            copy_out(p + done, k);
            done += k;
            wake_writer = true;
        }
        lock.unlock();
        if (wake_writer)
            space_cv_.notify_one();
        return static_cast<int>(done);
    }

    // 阻塞式读取：分块读满 n，直到完成、被 notify_reader() 提示或写端 finish 后读到 EOF
    int read(void *buf, size_t n)
    {
        if (n == 0)
            return 0;
        if (n > static_cast<size_t>(INT_MAX))
            return fail_read(EINVAL);

        auto p = static_cast<unsigned char *>(buf);
        size_t done = 0;
        bool wake_writer = false;
        std::unique_lock lock{mutex_};
        if (read_finished())
            return fail_read(EPIPE);
        while (done < n)
        {
            if (write_finished() && used_ == 0)
                break; // 排空后 EOF，done 可为 0
            if (reader_notified_)
            {
                reader_notified_ = false;
                size_t k = used_ < n - done ? used_ : n - done;
                if (k)
                {
                    copy_out(p + done, k);
                    done += k;
                    wake_writer = true;
                }
                break;
            }
            if (used_ == 0)
            {
                if (wake_writer)
                {
                    space_cv_.notify_one();
                    wake_writer = false;
                }
                data_cv_.wait(lock, [this] {
                    return write_finished() || reader_notified_ || used_ != 0;
                });
                continue;
            }
            size_t k = used_ < n - done ? used_ : n - done;
            copy_out(p + done, k);
            done += k;
            wake_writer = true;
        }
        lock.unlock();
        if (wake_writer)
            space_cv_.notify_one();
        return static_cast<int>(done);
    }

    // 提示写端提前退出：当前/下一次 write 停止等待，返回已写字节数
    void notify_writer()
    {
        {
            std::lock_guard lock{mutex_};
            writer_notified_ = true;
        }
        space_cv_.notify_all();
    }

    // 永久结束读端：阻塞写者返回已写字节数；之后的新写和读返回 -1/EPIPE
    void finish_read()
    {
        {
            std::lock_guard lock{mutex_};
            writer_notified_ = false;
            read_finished_.store(true, std::memory_order_relaxed);
        }
        space_cv_.notify_all();
    }

    // 读端是否已永久结束；无锁查询
    bool read_finished() const noexcept
    {
        return read_finished_.load(std::memory_order_relaxed);
    }

    // 清空缓冲数据；不清提示标志、finish 状态和 errno
    void clear()
    {
        {
            std::lock_guard lock{mutex_};
            used_ = 0;
            read_pos_ = 0;
            write_pos_ = 0;
        }
        space_cv_.notify_all();
    }

    // 完整复位：清数据、提示、finish 状态和 errno；调用时读、写两端必须都空闲（使用者保证）
    void reset()
    {
        std::lock_guard lock{mutex_};
        used_ = 0;
        read_pos_ = 0;
        write_pos_ = 0;
        reader_notified_ = false;
        writer_notified_ = false;
        write_finished_.store(false, std::memory_order_relaxed);
        read_finished_.store(false, std::memory_order_relaxed);
        read_errno_ = 0;
        write_errno_ = 0;
    }

    // 读侧最近一次失败的错误码；只由读线程查询，成功不清零
    int read_errno() const noexcept
    {
        return read_errno_;
    }

    // 写侧最近一次失败的错误码；只由写线程查询，成功不清零
    int write_errno() const noexcept
    {
        return write_errno_;
    }
};

// kfifo：单中断写 + 单任务读的字节环形缓冲。写端是中断，所以只保留非阻塞的
// try_write；读端有 try_read/read_for/read，读满 n、EOF 或超时才返回。
//
// 上下文：
//   - 写端（try_write/finish_write/write_errno/两个 finish 查询）可以在中断里调用，
//     中断的优先级数值必须 >= configMAX_SYSCALL_INTERRUPT_PRIORITY；写端必须是
//     单一、不可重入的上下文（不要从更高优先级中断再调写端）。
//   - 读端阻塞在 futex 上，read/read_for 只能在任务里调用；读端只能是一个任务。
//     clear()/reset() 也由读端调用（reset 要求写端空闲）。
//   - 依赖 xeno_thread 后端（thread_single 的 futex 是桩，且不链接 FreeRTOS port 对象）。
//
// 临界区只做快照和提交，数据拷贝在 critical_lock 之外完成：SPSC 下写端只碰
// [write_pos_, write_pos_+free)，读端只碰 [read_pos_, read_pos_+used)，两区间永不相交；
// 未提交的写对读端不可见，未提交的读只让写端少看到空间。因此生产者中断的延迟与 N 无关。
//
// 内存序依赖单核、无 DMA：read_pos_/write_pos_/used_ 只在 critical_lock 内访问，
// 锁的 BASEPRI 宏带 "memory" 屏障，锁外拷贝不会越过提交；event_ 是唯一的发布点
// （锁内 release 存，读者 acquire）。多核或 DMA 场景需要重新设计。
template <size_t N>
class kfifo
{
    static_assert(N > 0, "kfifo: N must be positive");
    static_assert(N <= static_cast<size_t>(INT_MAX), "kfifo: N must fit int");

    std::array<unsigned char, N> buffer_;
    size_t read_pos_ = 0;           // 读游标，[0, N)
    size_t write_pos_ = 0;          // 写游标，[0, N)
    size_t used_ = 0;               // 已提交字节数，[0, N]
    std::atomic<bool> write_finished_{false};  // 写端永久结束
    std::atomic<bool> read_finished_{false};   // 读端永久结束
    int read_errno_ = 0;
    int write_errno_ = 0;
    int event_ = 0;                 // futex 事件字；写端在环形锁内推进

    size_t free_space() const
    {
        return N - used_;
    }

    int fail_read(int e)
    {
        read_errno_ = e;
        return -1;
    }

    int fail_write(int e)
    {
        write_errno_ = e;
        return -1;
    }

    static size_t ring_add(size_t pos, size_t n) noexcept
    {
        pos += n;
        if (pos >= N)
            pos -= N;
        return pos;
    }

    // 以下只做跨尾 memcpy，不改游标/used_；start ∈ [0, N)，0 <= n <= N
    void copy_in_at(size_t start, const void *buf, size_t n)
    {
        if (n == 0)
            return;
        auto p = static_cast<const unsigned char *>(buf);
        size_t first = n < N - start ? n : N - start;
        std::memcpy(buffer_.data() + start, p, first);
        if (n > first)
            std::memcpy(buffer_.data(), p + first, n - first);
    }

    void copy_out_at(size_t start, void *buf, size_t n) const
    {
        if (n == 0)
            return;
        auto p = static_cast<unsigned char *>(buf);
        size_t first = n < N - start ? n : N - start;
        std::memcpy(p, buffer_.data() + start, first);
        if (n > first)
            std::memcpy(p + first, buffer_.data(), n - first);
    }

    // 以下要求持环形锁；拷贝已在锁外完成，这里只提交索引
    void commit_write(size_t start, size_t k)
    {
        write_pos_ = ring_add(start, k);
        used_ += k;
    }

    void commit_read(size_t start, size_t k)
    {
        read_pos_ = ring_add(start, k);
        used_ -= k;
    }

    // 写端在环形锁内推进事件字，返回值用于解锁后的 __futex_wake
    int bump_event_locked()
    {
        unsigned v = static_cast<unsigned>(__atomic_load_n(&event_, __ATOMIC_RELAXED));
        int next = static_cast<int>(v + 1u);
        __atomic_store_n(&event_, next, __ATOMIC_RELEASE);
        return next;
    }

public:

    // 尝试写入：写当前能写的字节数，返回实际写入数；完全没空间时 -1/EAGAIN
    int try_write(const void *buf, size_t n)
    {
        if (n == 0)
            return 0;
        if (n > static_cast<size_t>(INT_MAX))
            return fail_write(EINVAL);

        size_t start, k;
        {
            critical_lock lock;
            if (write_finished() || read_finished())
                return fail_write(EPIPE);
            k = free_space() < n ? free_space() : n;
            if (k == 0)
                return fail_write(EAGAIN);
            start = write_pos_;
        }

        // 锁外拷贝；写端单一且不可重入，这块空间不会被别人占走
        copy_in_at(start, buf, k);

        int next;
        {
            critical_lock lock;
            commit_write(start, k);
            next = bump_event_locked();
        }
        __futex_wake(&event_, next);
        return static_cast<int>(k);
    }

    // 永久结束写端：读端排空缓冲后得到 EOF（read 返回 0）；之后的写返回 -1/EPIPE
    void finish_write()
    {
        int next;
        {
            critical_lock lock;
            write_finished_.store(true, std::memory_order_relaxed);
            next = bump_event_locked();
        }
        __futex_wake(&event_, next);
    }

    // 写端是否已永久结束；无锁查询
    bool write_finished() const noexcept
    {
        return write_finished_.load(std::memory_order_relaxed);
    }

    // 尝试读取：读当前能读的字节数；排空后的 0 是 EOF，-1/EAGAIN 表示暂时没数据
    int try_read(void *buf, size_t n)
    {
        if (n == 0)
            return 0;
        if (n > static_cast<size_t>(INT_MAX))
            return fail_read(EINVAL);

        size_t start, k;
        {
            critical_lock lock;
            if (read_finished())
                return fail_read(EPIPE);
            if (write_finished() && used_ == 0)
                return 0; // EOF
            k = used_ < n ? used_ : n;
            if (k == 0)
                return fail_read(EAGAIN);
            start = read_pos_;
        }

        copy_out_at(start, buf, k);

        {
            critical_lock lock;
            commit_read(start, k);
        }
        return static_cast<int>(k);
    }

    // 指定时间内尝试读取：尽量读满 n；超时时返回已读出数，零进展才 -1/ETIMEDOUT
    template <typename Rep, typename Period>
    int read_for(const std::chrono::duration<Rep, Period> &d, void *buf, size_t n)
    {
        if (n == 0)
            return 0;
        if (n > static_cast<size_t>(INT_MAX))
            return fail_read(EINVAL);
        if (read_finished())
            return fail_read(EPIPE);

        auto p = static_cast<unsigned char *>(buf);
        size_t done = 0;
        bool expired = false;
        auto deadline = std::chrono::steady_clock::now() + d;

        for (;;)
        {
            int expected = 0;
            size_t start = 0, k = 0;
            bool wait = false;
            {
                critical_lock lock;
                if (write_finished() && used_ == 0)
                    return static_cast<int>(done); // 排空后 EOF，done 可为 0
                if (used_ != 0)
                {
                    k = used_ < n - done ? used_ : n - done;
                    start = read_pos_;
                }
                else if (expired || std::chrono::steady_clock::now() >= deadline)
                {
                    // 没有数据且要等：到这里才看截止时间
                    if (done == 0)
                        return fail_read(ETIMEDOUT);
                    return static_cast<int>(done);
                }
                else
                {
                    expected = __atomic_load_n(&event_, __ATOMIC_ACQUIRE);
                    wait = true;
                }
            }

            if (wait)
            {
                auto remain = deadline - std::chrono::steady_clock::now();
                if (remain <= std::chrono::steady_clock::duration::zero())
                {
                    expired = true;
                    continue;
                }
                // 正数不足 1ms 向上取整，避免在截止前被误判超时
                auto ms = std::chrono::ceil<std::chrono::milliseconds>(remain).count();
                if (ms < 1)
                    ms = 1;
                if (__futex_timedwait(&event_, expected, ms) == ETIMEDOUT)
                    expired = true;
                continue;
            }

            copy_out_at(start, p + done, k);

            {
                critical_lock lock;
                commit_read(start, k);
            }
            done += k;
            if (done == n)
                return static_cast<int>(done);
        }
    }

    // 阻塞式读取：分块读满 n，直到完成或写端 finish 后读到 EOF
    int read(void *buf, size_t n)
    {
        if (n == 0)
            return 0;
        if (n > static_cast<size_t>(INT_MAX))
            return fail_read(EINVAL);
        if (read_finished())
            return fail_read(EPIPE);

        auto p = static_cast<unsigned char *>(buf);
        size_t done = 0;

        for (;;)
        {
            int expected = 0;
            size_t start = 0, k = 0;
            bool wait = false;
            {
                critical_lock lock;
                if (write_finished() && used_ == 0)
                    return static_cast<int>(done); // 排空后 EOF，done 可为 0
                if (used_ != 0)
                {
                    k = used_ < n - done ? used_ : n - done;
                    start = read_pos_;
                }
                else
                {
                    expected = __atomic_load_n(&event_, __ATOMIC_ACQUIRE);
                    wait = true;
                }
            }

            if (wait)
            {
                __futex_wait(&event_, expected);
                continue;
            }

            copy_out_at(start, p + done, k);

            {
                critical_lock lock;
                commit_read(start, k);
            }
            done += k;
            if (done == n)
                return static_cast<int>(done);
        }
    }

    // 永久结束读端：之后的新写和读返回 -1/EPIPE；不唤醒（读端只有自己）
    void finish_read()
    {
        critical_lock lock;
        read_finished_.store(true, std::memory_order_relaxed);
    }

    // 读端是否已永久结束；无锁查询
    bool read_finished() const noexcept
    {
        return read_finished_.load(std::memory_order_relaxed);
    }

    // 清空缓冲数据；不动 finish 状态和 errno；只由读端调用
    void clear()
    {
        critical_lock lock;
        used_ = 0;
        read_pos_ = 0;
        write_pos_ = 0;
    }

    // 完整复位：清数据、finish 状态和 errno；调用时读、写两端必须都空闲（使用者保证）
    void reset()
    {
        critical_lock lock;
        used_ = 0;
        read_pos_ = 0;
        write_pos_ = 0;
        write_finished_.store(false, std::memory_order_relaxed);
        read_finished_.store(false, std::memory_order_relaxed);
        read_errno_ = 0;
        write_errno_ = 0;
    }

    // 读侧最近一次失败的错误码；只由读线程查询，成功不清零
    int read_errno() const noexcept
    {
        return read_errno_;
    }

    // 写侧最近一次失败的错误码；只由写侧查询，成功不清零
    int write_errno() const noexcept
    {
        return write_errno_;
    }
};


struct sched_suspend_guard
{
    bool flag;
    sched_suspend_guard()
    {
        flag = __scheduler_suspend();
    }

    ~sched_suspend_guard()
    {
        if(flag)
            __scheduler_resume();
    }
};

struct irq_disable_guard
{
    bool flag;
    irq_disable_guard()
    {
        flag = __irq_disable();
    }

    ~irq_disable_guard()
    {
        if(flag)
            __irq_enable();
    }
};

inline struct timespec timespec_diff(const struct timespec *start, const struct timespec *end)
{
    struct timespec temp;
    
    if ((end->tv_nsec - start->tv_nsec) < 0) {
        temp.tv_sec = end->tv_sec - start->tv_sec - 1;
        temp.tv_nsec = 1000000000 + end->tv_nsec - start->tv_nsec;
    } else {
        temp.tv_sec = end->tv_sec - start->tv_sec;
        temp.tv_nsec = end->tv_nsec - start->tv_nsec;
    }
    
    return temp;
}

void print_variable(auto x)
{
    char buf[32] = {};
    std::to_chars(std::begin(buf), std::end(buf), x, 16);
    __write_terminal(buf);
    __write_terminal("\r\n");
}

void printk(auto &&...args)
{
    char s[256];
    snprintf(s, sizeof s, args...);
    __write_terminal(s);
}

}
#endif