#include<FreeRTOS.h>
#include<task.h>

#include<xeno/kits.h>

extern "C" [[gnu::weak]]
void vApplicationStackOverflowHook(TaskHandle_t xTask, char * pcTaskName)
{
    xeno::printk("stackoverflow checked!\r\n");
    xeno::printk("task %p, name %s", xTask, pcTaskName);
    abort();
}