/* client/xbox/syscall.c
 *
 * Xbox kosload syscall dispatcher.
 *
 */

#include <kosload/syscall_table.h>

int kosloadsyscall(int nr, int a1, int a2, int a3) {
    if((unsigned int)nr >= kosload_syscall_count)
        return -1;

    /* Table entries are typed as void(*)(void); every kosload syscall handler
     * takes at most three int-sized args and returns an int/pointer in eax.
     * cdecl lets the caller pass three args regardless of the callee arity. */
    int (*handler)(int, int, int) = (int (*)(int, int, int))kosload_syscall_table[nr];
    return handler(a1, a2, a3);
}
