#include"stacktrace.h"
#include<unwind.h>
#include<stdint.h>
#include<unistd.h>
#include<string.h>
#include<stdlib.h>

#include<iterator>

static _Unwind_Reason_Code backtrace_callback(struct _Unwind_Context* context, void* arg)
{
    uintptr_t pc = _Unwind_GetIP(context);
    if (pc == 0)
        return _URC_END_OF_STACK;
    
    stacktrace_array *parray = static_cast<stacktrace_array *>(arg);
    if(parray->n >= (int)std::size(parray->a))
        return _URC_END_OF_STACK;

    parray->a[parray->n] = pc - 1;
    parray->n++;
    
    return _URC_NO_REASON;
}

void store_current_stacktrace(stacktrace_array *parray)
{
    parray->n = 0;
    _Unwind_Backtrace(backtrace_callback, parray);
}

static void write_terminal(const char *s)
{
    __write_terminal(s);
}

void print_stacktrace_array(const stacktrace_array *parray)
{
    write_terminal("addr2line -i -p -C -f -e main ");
    for(int i = 0; i < parray->n; i++)
    {
        char buf[8 * 2 + 1] = {};
        std::to_chars(std::begin(buf), std::end(buf), parray->a[i], 16);
        write_terminal("0x");
        write_terminal(buf);
        write_terminal(" ");
    }
    write_terminal("\r\n");
}

void print_stacktrace()
{
    stacktrace_array array;
    store_current_stacktrace(&array);
    print_stacktrace_array(&array);
}
