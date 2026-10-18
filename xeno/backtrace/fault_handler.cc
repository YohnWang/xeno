#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <charconv>
#include <iterator>
#include <unwind.h>

#include <stacktrace.h>

/*
 * Cortex-M fault backtrace.  See README.md in this directory for the full
 * exception-frame / synthetic-frame design.
 */

/*
 * Cortex-M exception frame, core part.  The hardware always pushes these
 * eight words first; an FP extended frame only appends S0-S15/FPSCR after
 * xPSR, so the layout below is valid for both frame kinds.
 */
struct exception_frame_t
{
    uintptr_t r0;
    uintptr_t r1;
    uintptr_t r2;
    uintptr_t r3;
    uintptr_t r12;
    uintptr_t lr;
    uintptr_t pc;
    uintptr_t xpsr;
};

/*
 * Callee-saved registers saved by fault_entry.S.  Order must match the
 * stmia/str in that file: r4-r8, r10, r11, then r9.
 */
struct callee_saved_t
{
    uintptr_t r4;
    uintptr_t r5;
    uintptr_t r6;
    uintptr_t r7;
    uintptr_t r8;
    uintptr_t r10;
    uintptr_t r11;
    uintptr_t r9;
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

static const char *fault_name(uintptr_t id)
{
    switch(id)
    {
        case 0: return "HardFault_Handler";
        case 1: return "MemManage_Handler";
        case 2: return "BusFault_Handler";
        case 3: return "UsageFault_Handler";
        default: return "Fault_Handler";
    }
}

/*
 * The unwinder starts at the handler frames (this function's caller chain)
 * and only reaches the interrupted context after fault_entry.S has turned
 * the hardware exception frame into a synthetic caller frame.  Skip
 * everything until the stacked PC shows up, then record the return
 * addresses as usual.
 */
struct fault_trace_context
{
    stacktrace_array *out;
    uintptr_t fault_pc;
    uintptr_t last_pc;
    bool reached_fault;
};

static _Unwind_Reason_Code fault_trace_callback(struct _Unwind_Context *context,
                                                void *arg)
{
    auto *ctx = static_cast<fault_trace_context *>(arg);
    uintptr_t pc = static_cast<uintptr_t>(_Unwind_GetIP(context));

    if(!ctx->reached_fault)
    {
        if(pc == ctx->fault_pc)
            ctx->reached_fault = true;
        return _URC_NO_REASON;
    }

    if(pc == 0 || ctx->out->n >= static_cast<int>(std::size(ctx->out->a)))
        return _URC_END_OF_STACK;

    /* Once the thread entry trampoline is reached there is no real caller
       chain left; EHABI can end up walking the same frame forever. */
    if(ctx->out->n > 1 && pc == ctx->last_pc)
        return _URC_END_OF_STACK;
    ctx->last_pc = pc;

    ctx->out->a[ctx->out->n++] = pc - 1;
    return _URC_NO_REASON;
}

static void store_fault_stacktrace(stacktrace_array *out, uintptr_t fault_pc)
{
    out->n = 0;

    /* Always report the faulting instruction, even if its function has no
       unwind record (GCC emits .cantunwind for many leaf functions). */
    const uintptr_t normalized = fault_pc & ~uintptr_t(1);
    out->a[out->n++] = normalized;

    fault_trace_context ctx{out, normalized, 0, false};
    _Unwind_Backtrace(fault_trace_callback, &ctx);
}

/* Called from fault_entry.S with exactly this argument layout. */
[[noreturn, gnu::used, gnu::noinline]]
void fault_handler_c(exception_frame_t *frame, uintptr_t exc_return,
                     uintptr_t fault_id, const callee_saved_t *saved)
{
    const bool fp_frame = (exc_return & 0x10) == 0;
    const uintptr_t frame_size = fp_frame ? 0x68 : 0x20;
    const uintptr_t align_pad = (frame->xpsr & 0x200) ? 4 : 0;
    const uintptr_t interrupted_sp =
        reinterpret_cast<uintptr_t>(frame) + frame_size + align_pad;

    write_terminal("\r\n========== ");
    write_terminal(fault_name(fault_id));
    write_terminal(" ==========\r\n");

    print_register("R0", frame->r0);
    print_register("R1", frame->r1);
    print_register("R2", frame->r2);
    print_register("R3", frame->r3);
    print_register("R4", saved->r4);
    print_register("R5", saved->r5);
    print_register("R6", saved->r6);
    print_register("R7", saved->r7);
    print_register("R8", saved->r8);
    print_register("R9", saved->r9);
    print_register("R10", saved->r10);
    print_register("R11", saved->r11);
    print_register("R12", frame->r12);
    print_register("SP", interrupted_sp);
    print_register("LR", frame->lr);
    print_register("PC", frame->pc);
    print_register("PSR", frame->xpsr);

    if(fp_frame)
        write_terminal("(FP extended frame)\r\n");

    stacktrace_array array;
    store_fault_stacktrace(&array, frame->pc);
    print_stacktrace_array(&array);

    while(1)
    {
    }
    __builtin_unreachable();
}

}
