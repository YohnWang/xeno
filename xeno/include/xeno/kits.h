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

namespace xeno
{

template <typename T, size_t N>
class static_queue
{
    static constexpr size_t queue_size = N;
    std::array<T, queue_size> a;
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