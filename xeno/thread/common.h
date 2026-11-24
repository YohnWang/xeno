#pragma once

#include<FreeRTOS.h>
#include<task.h>
#include<xeno/critical.h>

namespace xeno
{

class scheduler_lock
{
public:
    void lock()
    {
        vTaskSuspendAll();
    }

    void unlock()
    {
        xTaskResumeAll();
    }
};

}
