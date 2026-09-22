#pragma once

#include<sys/features.h>

#include_next<bits/c++config.h>

#define _GLIBCXX_HAVE_POSIX_SEMAPHORE 1 
#define _GLIBCXX_USE_POSIX_SEMAPHORE 1
#define _GLIBCXX_HAS_GTHREADS
#define _GLIBCXX_HAVE_TLS 1
#define _GLIBCXX_USE_PTHREAD_COND_CLOCKWAIT 1