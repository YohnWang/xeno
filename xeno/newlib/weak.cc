#include<stacktrace.h>

[[gnu::weak]] void print_stacktrace(){}
[[gnu::weak]] void store_current_stacktrace(stacktrace_array *parray){}
[[gnu::weak]] void print_stacktrace_array(const stacktrace_array *parray){}
[[gnu::weak]] void print_current_exception_stacktrace(){}
