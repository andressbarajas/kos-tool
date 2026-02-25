/* host/include/kostool/syscalls.h */
#ifndef KOSTOOL_SYSCALLS_H
#define KOSTOOL_SYSCALLS_H

#include "context.h"

/* Host-side syscall handler interface.
 * These implement the 23 syscalls that the console can request.
 */

int syscall_fstat(kostool_context_t *ctx);
int syscall_write(kostool_context_t *ctx);
int syscall_read(kostool_context_t *ctx);
int syscall_open(kostool_context_t *ctx);
int syscall_close(kostool_context_t *ctx);
int syscall_creat(kostool_context_t *ctx);
int syscall_link(kostool_context_t *ctx);
int syscall_unlink(kostool_context_t *ctx);
int syscall_chdir(kostool_context_t *ctx);
int syscall_chmod(kostool_context_t *ctx);
int syscall_lseek(kostool_context_t *ctx);
int syscall_time(kostool_context_t *ctx);
int syscall_stat(kostool_context_t *ctx);
int syscall_utime(kostool_context_t *ctx);
int syscall_opendir(kostool_context_t *ctx);
int syscall_closedir(kostool_context_t *ctx);
int syscall_readdir(kostool_context_t *ctx);
int syscall_rewinddir(kostool_context_t *ctx);
int syscall_cdfs_read_sectors(kostool_context_t *ctx);
int syscall_gdbpacket(kostool_context_t *ctx);
int syscall_exit(kostool_context_t *ctx);
int syscall_mkdir(kostool_context_t *ctx);

#endif /* KOSTOOL_SYSCALLS_H */
