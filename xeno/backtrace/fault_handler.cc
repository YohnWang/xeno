#include <stdio.h>
#include <reent.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <charconv>
#include <iterator>
#include <algorithm>
#include <stacktrace.h>

struct exception_frame_t{
    uintptr_t r4;
    uintptr_t r5;
    uintptr_t r6;
    uintptr_t r7;
    uintptr_t r8;
    uintptr_t r9;
    uintptr_t r10;
    uintptr_t r11;
    uintptr_t r0;
    uintptr_t r1;
    uintptr_t r2;
    uintptr_t r3;
    uintptr_t r12;
    uintptr_t lr;
    uintptr_t pc;
    uintptr_t psr;
};

extern "C"
{

static void write_terminal(const char *s)
{
    __write_terminal(s);
}

static void print_register(const char * reg, uintptr_t value)
{
    write_terminal(reg);
    if(strlen(reg) == 2)
        write_terminal(" ");
    write_terminal(" = ");
    char buf[16] = {};
    std::to_chars(std::begin(buf), std::end(buf), value, 16);
    write_terminal(buf);
    write_terminal("\r\n");
}

static void print_registers(exception_frame_t *stack_frame)
{
    print_register("R0", stack_frame->r0);
    print_register("R1", stack_frame->r1);
    print_register("R2", stack_frame->r2);
    print_register("R3", stack_frame->r3);
    print_register("R4", stack_frame->r4);
    print_register("R5", stack_frame->r5);
    print_register("R6", stack_frame->r6);
    print_register("R7", stack_frame->r7);
    print_register("R8", stack_frame->r8);
    print_register("R9", stack_frame->r9);
    print_register("R10", stack_frame->r10);
    print_register("R11", stack_frame->r11);
    print_register("R12", stack_frame->r12);
    print_register("SP", (uintptr_t)stack_frame);
    print_register("LR", stack_frame->lr);
    print_register("PC", stack_frame->pc);
    print_register("PSR", stack_frame->psr);
}

__attribute__((always_inline))
static inline void get_sp_to_r0()
{
    __asm__ volatile (R"(
        tst lr, #4
        ite eq
        mrseq r0, msp
        mrsne r0, psp
        sub r0, #32
        str r4,  [r0, #0]
        str r5,  [r0, #4]
        str r6,  [r0, #8]
        str r7,  [r0, #12]
        str r8,  [r0, #16]
        str r9,  [r0, #20]
        str r10, [r0, #24]
        str r11, [r0, #28]
    )");
}

[[noreturn, gnu::used]]
void HardFault_Handler_C(exception_frame_t *stack_frame)
{
    write_terminal("\r\n========== HardFault_Handler ==========\r\n");
    print_registers(stack_frame);
    while (1);
    __builtin_unreachable();
}

__attribute__((section(".after_vectors"), naked))
void HardFault_Handler(void)
{
    get_sp_to_r0();
    __asm__ volatile ("b HardFault_Handler_C");
}

[[noreturn, gnu::used]]
void MemManage_Handler_C(exception_frame_t *stack_frame)
{
    write_terminal("\r\n========== MemManage_Handler ==========\r\n");
    print_registers(stack_frame);
    while (1);
    __builtin_unreachable();
}

__attribute__((section(".after_vectors"), naked))
void MemManage_Handler(void)
{
    get_sp_to_r0();
    __asm__ volatile ("b MemManage_Handler_C");
}

[[noreturn, gnu::used]]
void BusFault_Handler_C(exception_frame_t *stack_frame)
{
    write_terminal("\r\n========== BusFault_Handler ==========\r\n");
    print_registers(stack_frame);
    while (1);
    __builtin_unreachable();
}

__attribute__((section(".after_vectors"), naked))
void BusFault_Handler(void)
{
    get_sp_to_r0();
    __asm__ volatile ("b BusFault_Handler_C");
}

[[noreturn, gnu::used]]
void UsageFault_Handler_C(exception_frame_t *stack_frame)
{
    write_terminal("\r\n========== UsageFault_Handler ==========\r\n");
    print_registers(stack_frame);
    while (1);
    __builtin_unreachable();
}

__attribute__((section(".after_vectors"), naked))
void UsageFault_Handler(void)
{
    get_sp_to_r0();
    __asm__ volatile ("b UsageFault_Handler_C");
}

}