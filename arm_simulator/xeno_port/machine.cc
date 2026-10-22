#include<stdint.h>
#include<xeno/port.h>

extern "C" bool __is_interrupted()
{
    uint32_t ipsr;
    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    return ipsr != 0;
}
