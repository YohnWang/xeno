#include <semaphore.h>

int sem_init(sem_t *__sem, int __pshared, unsigned int __value)
{
    return 0;
}

int sem_destroy(sem_t *__sem)
{
    return 0;
}

int sem_wait(sem_t *__sem)
{
    return 0;
}

int sem_timedwait(sem_t *__restrict __sem, const struct timespec *__restrict __abstime)
{
    return -1;
}

int sem_trywait(sem_t *__sem)
{
    return -1;
}

int sem_post(sem_t *__sem)
{
    return -1;
}

int sem_getvalue(sem_t *__restrict __sem, int *__restrict __sval)
{
    return -1;
}
