/* client/include/kosload/syscall_table.h */
/*
 * Kosload syscall table interface.
 *
 * The syscall table and count are defined in C (syscall_table.c)
 * and referenced by the ASM dispatcher in crt0.S. Adding a new
 * syscall only requires adding it to the C array — no ASM changes.
 */

#ifndef KOSLOAD_SYSCALL_TABLE_H
#define KOSLOAD_SYSCALL_TABLE_H

typedef void (*syscall_fn_t)(void);

extern const syscall_fn_t kosload_syscall_table[];
extern const unsigned int kosload_syscall_count;

#endif /* KOSLOAD_SYSCALL_TABLE_H */
