#ifndef _SEMAPHORE_H
#define _SEMAPHORE_H	1

#include <sys/types.h>

#include<stdint.h>
#include<limits.h>

#include<FreeRTOS.h>
#include<semphr.h>

#define SEM_VALUE_MAX INT_MAX

typedef struct sem_t
{
    SemaphoreHandle_t handle;
}sem_t;

__BEGIN_DECLS

/* Initialize semaphore object SEM to VALUE.  If PSHARED then share it
   with other processes.  */
extern int sem_init (sem_t *__sem, int __pshared, unsigned int __value)
  __THROW __nonnull ((1));

/* Free resources associated with semaphore object SEM.  */
extern int sem_destroy (sem_t *__sem) __THROW __nonnull ((1));

/* Open a named semaphore NAME with open flags OFLAG.  */
extern sem_t *sem_open (const char *__name, int __oflag, ...)
  __THROW __nonnull ((1));

/* Close descriptor for named semaphore SEM.  */
extern int sem_close (sem_t *__sem) __THROW __nonnull ((1));

/* Remove named semaphore NAME.  */
extern int sem_unlink (const char *__name) __THROW __nonnull ((1));

/* Wait for SEM being posted.

   This function is a cancellation point and therefore not marked with
   __THROW.  */
extern int sem_wait (sem_t *__sem) __nonnull ((1));


extern int sem_timedwait (sem_t *__restrict __sem,
			  const struct timespec *__restrict __abstime)
  __nonnull ((1, 2));


/* Test whether SEM is posted.  */
extern int sem_trywait (sem_t *__sem) __nonnull ((1));

/* Post SEM.  */
extern int sem_post (sem_t *__sem) __nonnull ((1));

/* Get current value of SEM and store it in *SVAL.  */
extern int sem_getvalue (sem_t *__restrict __sem, int *__restrict __sval)
  __THROW __nonnull ((1, 2));


__END_DECLS

#endif	/* semaphore.h */
