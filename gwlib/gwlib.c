/*
 * gwlib.c - definition of the gwlib_init and gwlib_shutdown functions
 *
 * Lars Wirzenius
 */

#include "gwlib.h"


/*
 * Has gwlib been initialized?
 */
static int init = 0;


void (gwlib_assert_init)(void)
{
    gw_assert(init != 0);
}


void gwlib_init(void) 
{
    gw_assert(!init);
    gw_init_mem();
    octstr_init();
    gwlib_protected_init();
    gwthread_init();
    http_init();
    socket_init();
    charset_init();
    init = 1;
}

void gwlib_shutdown(void) 
{
    gwlib_assert_init();
    charset_shutdown();
    http_shutdown();
    socket_shutdown();
    gwthread_shutdown();
    octstr_shutdown();
    gw_check_leaks();
    gwmem_shutdown();
    gwlib_protected_shutdown();
    init = 0;
}