#include<sched.h>
#include<FreeRTOS.h>
#include<task.h>

int sched_get_priority_max( int policy )
{
    /* Silence warnings about unused parameters. */
    ( void ) policy;

    return configMAX_PRIORITIES - 1;
}

/*-----------------------------------------------------------*/

int sched_get_priority_min( int policy )
{
    /* Silence warnings about unused parameters. */
    ( void ) policy;

    return tskIDLE_PRIORITY;
}

/*-----------------------------------------------------------*/

int sched_yield( void )
{
    taskYIELD();

    return 0;
}

/*-----------------------------------------------------------*/
