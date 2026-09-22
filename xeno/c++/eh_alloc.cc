// 该实现取自libstdc++，用于替换libstdc++中的__cxa_allocate_exception，额外添加记录栈踪信息

#include<string.h>
#include<exception>
#include<cxxabi.h>
#include<stdlib.h>
#include<unwind.h>
#include<bits/atomic_word.h>

#include<stacktrace.h>

struct __cxa_exception
{
  // Manage the exception object itself.
  std::type_info *exceptionType;
  void (_GLIBCXX_CDTOR_CALLABI *exceptionDestructor)(void *);

  // The C++ standard has entertaining rules wrt calling set_terminate
  // and set_unexpected in the middle of the exception cleanup process.
  std::terminate_handler unexpectedHandler;
  std::terminate_handler terminateHandler;

  // The caught exception stack threads through here.
  __cxa_exception *nextException;

  // How many nested handlers have caught this exception.  A negated
  // value is a signal that this object has been rethrown.
  int handlerCount;

#ifdef __ARM_EABI_UNWINDER__
  // Stack of exceptions in cleanups.
  __cxa_exception* nextPropagatingException;

  // The number of active cleanup handlers for this exception.
  int propagationCount;
#else
  // Cache parsed handler data from the personality routine Phase 1
  // for Phase 2 and __cxa_call_unexpected.
  int handlerSwitchValue;
  const unsigned char *actionRecord;
  const unsigned char *languageSpecificData;
  _Unwind_Ptr catchTemp;
  void *adjustedPtr;
#endif

  // The generic exception header.  Must be last.
  _Unwind_Exception unwindHeader;
};

struct __cxa_refcounted_exception
{
  // Manage this header.
  _Atomic_word referenceCount;
  // __cxa_exception must be last, and no padding can be after it.
  __cxa_exception exc;
};

static void set_current_stacktrace() noexcept
{
    store_current_stacktrace(&__ext_current_exception_stacktrace);
}

extern "C" void *
__wrap___cxa_allocate_exception(std::size_t thrown_size) noexcept
{
  thrown_size += sizeof (__cxa_refcounted_exception);

  void *ret = malloc (thrown_size);

  set_current_stacktrace();

  if (!ret)
    std::terminate ();

  memset (ret, 0, sizeof (__cxa_refcounted_exception));

  return (void *)((char *)ret + sizeof (__cxa_refcounted_exception));
}

static void print_exception_stacktrace(const stacktrace_array *parray) noexcept
{
    __write_terminal("exception_stacktrace:\r\n  ");
    print_stacktrace_array(parray);
}

void print_current_exception_stacktrace()
{
    print_exception_stacktrace(&__ext_current_exception_stacktrace);
}
