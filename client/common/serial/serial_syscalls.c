/* client/common/serial/serial_syscalls.c */
/*
 * Serial transport syscall implementations for kosload client.
 * Based on dcload-serial: dcload-serial/target-src/dcload/syscalls.c
 *
 * These functions implement POSIX-like file I/O by sending single-byte
 * command IDs over the serial link, followed by LZO-compressed data
 * transfers with XOR checksums.
 *
 * The crt0.S syscall table maps slots 0-21 to these functions.
 */

#include <string.h>
#include <kosload/protocol.h>
#include <kosload/serial_io.h>
#include <kosload/types.h>
#include "serial_internal.h"

/* ===== Syscall implementations ===== */

/* Syscall 14: loaded programs call this to provide work memory for
 * LZO compression used by write() and other send syscalls. */
void assign_wrkmem(unsigned char *user_buffer) {
    wrkmem = user_buffer;
}

void progexit(int ret_code)
{
    serial_io_putchar(SERIAL_SYSCALL_PROGEXIT);
    put_uint(ret_code);
    serial_io_flush();
}

int read(int fd, void *buf, unsigned int count)
{
    serial_io_putchar(SERIAL_SYSCALL_READ);
    put_uint(fd);
    put_uint(count);
    load_data_block_general((unsigned char *)buf, count, 0);
    return get_uint();
}

int write(int fd, const void *buf, unsigned int count)
{
    serial_io_putchar(SERIAL_SYSCALL_WRITE);
    put_uint(fd);
    put_uint(count);
    send_data_block_compressed((unsigned char *)buf, count);
    return get_uint();
}

int open(const char *pathname, int flags, int mode)
{
    unsigned int namelen = strlen(pathname) + 1;

    serial_io_putchar(SERIAL_SYSCALL_OPEN);
    put_uint(namelen);
    send_data_block_compressed((unsigned char *)pathname, namelen);
    put_uint(flags);
    put_uint(mode);
    return get_uint();
}

int close(int fd)
{
    serial_io_putchar(SERIAL_SYSCALL_CLOSE);
    put_uint(fd);
    return get_uint();
}

int creat(const char *pathname, int mode)
{
    unsigned int namelen = strlen(pathname) + 1;

    serial_io_putchar(SERIAL_SYSCALL_CREAT);
    put_uint(namelen);
    send_data_block_compressed((unsigned char *)pathname, namelen);
    put_uint(mode);
    return get_uint();
}

int link(const char *oldpath, const char *newpath)
{
    unsigned int namelen1 = strlen(oldpath) + 1;
    unsigned int namelen2 = strlen(newpath) + 1;

    serial_io_putchar(SERIAL_SYSCALL_LINK);
    put_uint(namelen1);
    send_data_block_compressed((unsigned char *)oldpath, namelen1);
    put_uint(namelen2);
    send_data_block_compressed((unsigned char *)newpath, namelen2);
    return get_uint();
}

int unlink(const char *pathname)
{
    unsigned int namelen = strlen(pathname) + 1;

    serial_io_putchar(SERIAL_SYSCALL_UNLINK);
    put_uint(namelen);
    send_data_block_compressed((unsigned char *)pathname, namelen);
    return get_uint();
}

int chdir(const char *path)
{
    unsigned int namelen = strlen(path) + 1;

    serial_io_putchar(SERIAL_SYSCALL_CHDIR);
    put_uint(namelen);
    send_data_block_compressed((unsigned char *)path, namelen);
    return get_uint();
}

int chmod(const char *path, int mode)
{
    unsigned int namelen = strlen(path) + 1;

    serial_io_putchar(SERIAL_SYSCALL_CHMOD);
    put_uint(namelen);
    send_data_block_compressed((unsigned char *)path, namelen);
    put_uint(mode);
    return get_uint();
}

int mkdir(const char *path, int mode)
{
    unsigned int namelen = strlen(path) + 1;

    serial_io_putchar(SERIAL_SYSCALL_MKDIR);
    put_uint(namelen);
    send_data_block_compressed((unsigned char *)path, namelen);
    put_uint(mode);
    return get_uint();
}

int lseek(int fd, int offset, int whence)
{
    serial_io_putchar(SERIAL_SYSCALL_LSEEK);
    put_uint(fd);
    put_uint(offset);
    put_uint(whence);
    return get_uint();
}

int fstat(int fd, void *buf)
{
    dcload_stat_t *ds = (dcload_stat_t *)buf;
    unsigned int blksize_val, blocks_val;

    serial_io_putchar(SERIAL_SYSCALL_FSTAT);
    put_uint(fd);

    /* Host sends 13 stat fields as individual uint32s in order:
     * dev, ino, mode, nlink, uid, gid, rdev, size,
     * blksize, blocks, atime, mtime, ctime.
     * Map to dcload_stat_t layout (uint16 for dev/ino/nlink/uid/gid/rdev,
     * and blksize/blocks come after time fields with spare slots). */
    ds->st_dev = (unsigned short)get_uint();
    ds->st_ino = (unsigned short)get_uint();
    ds->st_mode = get_uint();
    ds->st_nlink = (unsigned short)get_uint();
    ds->st_uid = (unsigned short)get_uint();
    ds->st_gid = (unsigned short)get_uint();
    ds->st_rdev = (unsigned short)get_uint();
    ds->st_size = get_uint();
    blksize_val = get_uint();
    blocks_val = get_uint();
    ds->st_atime_val = get_uint();
    ds->st_spare1 = 0;
    ds->st_mtime_val = get_uint();
    ds->st_spare2 = 0;
    ds->st_ctime_val = get_uint();
    ds->st_spare3 = 0;
    ds->st_blksize = blksize_val;
    ds->st_blocks = blocks_val;
    ds->st_spare4[0] = 0;
    ds->st_spare4[1] = 0;

    return get_uint();
}

int time(unsigned int *t)
{
    unsigned int result;

    serial_io_putchar(SERIAL_SYSCALL_TIME);
    result = get_uint();

    if (t)
        *t = result;

    return result;
}

int stat(const char *pathname, void *buf)
{
    unsigned int namelen = strlen(pathname) + 1;
    dcload_stat_t *ds = (dcload_stat_t *)buf;
    unsigned int blksize_val, blocks_val;

    serial_io_putchar(SERIAL_SYSCALL_STAT);
    put_uint(namelen);
    send_data_block_compressed((unsigned char *)pathname, namelen);

    /* Host sends 13 stat fields as individual uint32s (same as fstat) */
    ds->st_dev = (unsigned short)get_uint();
    ds->st_ino = (unsigned short)get_uint();
    ds->st_mode = get_uint();
    ds->st_nlink = (unsigned short)get_uint();
    ds->st_uid = (unsigned short)get_uint();
    ds->st_gid = (unsigned short)get_uint();
    ds->st_rdev = (unsigned short)get_uint();
    ds->st_size = get_uint();
    blksize_val = get_uint();
    blocks_val = get_uint();
    ds->st_atime_val = get_uint();
    ds->st_spare1 = 0;
    ds->st_mtime_val = get_uint();
    ds->st_spare2 = 0;
    ds->st_ctime_val = get_uint();
    ds->st_spare3 = 0;
    ds->st_blksize = blksize_val;
    ds->st_blocks = blocks_val;
    ds->st_spare4[0] = 0;
    ds->st_spare4[1] = 0;

    return get_uint();
}

int utime(const char *filename, void *buf)
{
    unsigned int namelen = strlen(filename) + 1;
    unsigned int *times = (unsigned int *)buf;

    serial_io_putchar(SERIAL_SYSCALL_UTIME);
    put_uint(namelen);
    send_data_block_compressed((unsigned char *)filename, namelen);
    if (buf) {
        put_uint(1);
        put_uint(times[0]); /* actime */
        put_uint(times[1]); /* modtime */
    } else {
        put_uint(0);
    }
    return get_uint();
}

unsigned int opendir(const char *name)
{
    unsigned int namelen = strlen(name) + 1;

    serial_io_putchar(SERIAL_SYSCALL_OPENDIR);
    put_uint(namelen);
    send_data_block_compressed((unsigned char *)name, namelen);
    return get_uint();
}

int closedir(unsigned int dir)
{
    serial_io_putchar(SERIAL_SYSCALL_CLOSEDIR);
    put_uint(dir);
    return get_uint();
}

typedef struct {
    long d_ino;
    long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[256];
} dc_dirent_t;

static dc_dirent_t serial_our_dir;

void *readdir(unsigned int dir)
{
    unsigned int namelen;

    serial_io_putchar(SERIAL_SYSCALL_READDIR);
    put_uint(dir);

    /* Host sends: flag (0=end, 1=entry), then fields + name data */
    if (get_uint()) {
        serial_our_dir.d_ino = get_uint();
        serial_our_dir.d_off = get_uint();
        serial_our_dir.d_reclen = get_uint();
        serial_our_dir.d_type = get_uint();
        namelen = get_uint();
        load_data_block_general((unsigned char *)serial_our_dir.d_name, namelen, 0);
        return &serial_our_dir;
    }

    return 0;
}

unsigned int gdbpacket(const char *in_buf, unsigned int size_pack, char *out_buf)
{
    unsigned int in_size = size_pack >> 16;
    unsigned int out_size = size_pack & 0xffff;
    unsigned int ret_size;

    serial_io_putchar(SERIAL_SYSCALL_GDBPACKET);
    put_uint(in_size);
    put_uint(out_size);

    if (in_size)
        send_data_block_compressed((unsigned char *)in_buf, in_size);

    ret_size = get_uint();

    if (ret_size && ret_size <= out_size)
        load_data_block_general((unsigned char *)out_buf, ret_size, 0);

    return ret_size;
}

int rewinddir(unsigned int dir)
{
    serial_io_putchar(SERIAL_SYSCALL_REWINDDIR);
    put_uint(dir);
    return get_uint();
}

/* gethostinfo stub - not meaningful for serial transport but
 * required by the crt0.S syscall table to avoid link errors */
int gethostinfo(unsigned int *ip, unsigned int *port)
{
    *ip = 0;
    *port = 0;
    return 0;
}
