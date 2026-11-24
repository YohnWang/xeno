#pragma once

#include<FreeRTOS.h>
#include<task.h>
#include<xeno/port.h>

namespace xeno
{

// 临界区 guard，任务和中断都能用：任务里走 portENTER_CRITICAL（BASEPRI 同时
// 挡住 PendSV 和可调用 FreeRTOS API 的中断），中断里保存/恢复中断屏蔽。
//
// 必须在当前上下文的栈上单独构造，不能共享实例：isr 标志和保存的 mask 是
// guard 的成员。要是放在共享的 static 锁对象里，任务刚判定 isr=false、还没
// 屏蔽中断时被 ISR 抢占，ISR 用同一对象改写状态，任务恢复后就会走错误的分支，
// uxCriticalNesting 泄漏，之后再也解不开中断。
class critical_lock
{
public:
    critical_lock()
    {
        isr = __is_interrupted();
        if(isr)
            mask = portSET_INTERRUPT_MASK_FROM_ISR();
        else
            portENTER_CRITICAL();
    }

    ~critical_lock()
    {
        if(isr)
            portCLEAR_INTERRUPT_MASK_FROM_ISR(mask);
        else
            portEXIT_CRITICAL();
    }

    critical_lock(const critical_lock &) = delete;
    critical_lock &operator=(const critical_lock &) = delete;
    critical_lock(critical_lock &&) = delete;
    critical_lock &operator=(critical_lock &&) = delete;

private:
    bool isr = false;
    UBaseType_t mask = 0;
};

}
