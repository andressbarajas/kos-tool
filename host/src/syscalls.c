/* host/src/syscalls.c */
#include <stdio.h>
#include <stdint.h>

#include <kostool/syscalls.h>

/* Stub implementations for all 22 syscalls.
 * These will be fully implemented in Phase 6.
 */

int syscall_fstat(kostool_context_t *ctx) { (void)ctx; return -1; }
int syscall_write(kostool_context_t *ctx) { (void)ctx; return -1; }
int syscall_read(kostool_context_t *ctx) { (void)ctx; return -1; }
int syscall_open(kostool_context_t *ctx) { (void)ctx; return -1; }
int syscall_close(kostool_context_t *ctx) { (void)ctx; return -1; }
int syscall_creat(kostool_context_t *ctx) { (void)ctx; return -1; }
int syscall_link(kostool_context_t *ctx) { (void)ctx; return -1; }
int syscall_unlink(kostool_context_t *ctx) { (void)ctx; return -1; }
int syscall_chdir(kostool_context_t *ctx) { (void)ctx; return -1; }
int syscall_chmod(kostool_context_t *ctx) { (void)ctx; return -1; }
int syscall_lseek(kostool_context_t *ctx) { (void)ctx; return -1; }
int syscall_time(kostool_context_t *ctx) { (void)ctx; return -1; }
int syscall_stat(kostool_context_t *ctx) { (void)ctx; return -1; }
int syscall_utime(kostool_context_t *ctx) { (void)ctx; return -1; }
int syscall_opendir(kostool_context_t *ctx) { (void)ctx; return -1; }
int syscall_closedir(kostool_context_t *ctx) { (void)ctx; return -1; }
int syscall_readdir(kostool_context_t *ctx) { (void)ctx; return -1; }
int syscall_rewinddir(kostool_context_t *ctx) { (void)ctx; return -1; }
int syscall_cdfs_read_sectors(kostool_context_t *ctx) { (void)ctx; return -1; }
int syscall_gdbpacket(kostool_context_t *ctx) { (void)ctx; return -1; }
int syscall_exit(kostool_context_t *ctx) { (void)ctx; return -1; }
