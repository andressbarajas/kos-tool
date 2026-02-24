/* client/common/syscall_table.c */
/*
 * Kosload syscall jump table.
 *
 * Defines the function pointer array and count used by the ASM
 * dispatcher in crt0.S. To add a new syscall:
 *   1. Implement the function in the transport layer
 *   2. Add a forward declaration and table entry here
 *   3. That's it — the count updates automatically
 *
 * The syscall indices must match the legacy dcload protocol
 * (dcload-syscalls.h in the examples directory).
 */

#include <kosload/syscall_table.h>

/* Syscall implementations are provided by:
 *   Serial: serial_transport.c
 *   Network: network_syscalls.c
 * Both define the same function names with different implementations. */
extern int read(int, void *, unsigned int);
extern int write(int, const void *, unsigned int);
extern int open(const char *, int, int);
extern int close(int);
extern int creat(const char *, int);
extern int link(const char *, const char *);
extern int unlink(const char *);
extern int chdir(const char *);
extern int chmod(const char *, int);
extern int lseek(int, int, int);
extern int fstat(int, void *);
extern int time(unsigned int *);
extern int stat(const char *, void *);
extern int utime(const char *, void *);
extern void assign_wrkmem(unsigned char *);
extern void dcexit(int);
extern unsigned int opendir(const char *);
extern int closedir(unsigned int);
extern void *readdir(unsigned int);
extern int gethostinfo(unsigned int *, unsigned int *);
extern unsigned int gdbpacket(const char *, unsigned int, char *);
extern int rewinddir(unsigned int);

const syscall_fn_t kosload_syscall_table[] = {
    (syscall_fn_t)read,           /*  0 */
    (syscall_fn_t)write,          /*  1 */
    (syscall_fn_t)open,           /*  2 */
    (syscall_fn_t)close,          /*  3 */
    (syscall_fn_t)creat,          /*  4 */
    (syscall_fn_t)link,           /*  5 */
    (syscall_fn_t)unlink,         /*  6 */
    (syscall_fn_t)chdir,          /*  7 */
    (syscall_fn_t)chmod,          /*  8 */
    (syscall_fn_t)lseek,          /*  9 */
    (syscall_fn_t)fstat,          /* 10 */
    (syscall_fn_t)time,           /* 11 */
    (syscall_fn_t)stat,           /* 12 */
    (syscall_fn_t)utime,          /* 13 */
    (syscall_fn_t)assign_wrkmem,  /* 14 */
    (syscall_fn_t)dcexit,         /* 15 */
    (syscall_fn_t)opendir,        /* 16 */
    (syscall_fn_t)closedir,       /* 17 */
    (syscall_fn_t)readdir,        /* 18 */
    (syscall_fn_t)gethostinfo,    /* 19 */
    (syscall_fn_t)gdbpacket,      /* 20 */
    (syscall_fn_t)rewinddir,      /* 21 */
};

const unsigned int kosload_syscall_count =
    sizeof(kosload_syscall_table) / sizeof(kosload_syscall_table[0]);
