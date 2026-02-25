/* host/src/console.c */
/*
 * Console/fileserver loop for kostool.
 *
 * Implements two console paths:
 * - Serial: reads single-byte command IDs, parameters via send_uint/recv_uint
 * - Network: receives UDP command packets, dispatches by 4-byte command ID
 *
 * Both paths share the same underlying file I/O operations.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <utime.h>
#include <errno.h>

#ifndef _WIN32
#include <arpa/inet.h>
#else
#include <winsock2.h>
#include <windows.h>
#include <io.h>
#include <direct.h>
/* Windows has no POSIX link(); use CreateHardLinkA */
static inline int link(const char *oldpath, const char *newpath) {
    return CreateHardLinkA(newpath, oldpath, NULL) ? 0 : -1;
}
#define mkdir(path, mode) _mkdir(path)
#endif

#include <kosload/protocol.h>
#include <kosload/types.h>
#include <kostool/transport.h>
#include <kostool/platform.h>
#include <kostool/gdb.h>
#include <kostool/cdfs.h>
#include "minilzo.h"

#ifndef O_BINARY
#define O_BINARY 0
#endif

#define MAX_PATH_LEN 4096
#define MAX_SYSCALL_SIZE (32 * 1024 * 1024)  /* 32MB sanity cap for remote malloc */

/* ===== addr2line address decoding ===== */

#define ADDR2LINE_CACHE_SIZE 64

static struct {
    uint32_t addr;
    char decoded[128];
} addr_cache[ADDR2LINE_CACHE_SIZE];
static int addr_cache_count = 0;

static int is_hex_char(int c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static uint32_t hex_to_uint(const char *s, int len) {
    uint32_t val = 0;
    int i;
    for (i = 0; i < len; i++) {
        val <<= 4;
        if (s[i] >= '0' && s[i] <= '9') val |= s[i] - '0';
        else if (s[i] >= 'a' && s[i] <= 'f') val |= s[i] - 'a' + 10;
        else if (s[i] >= 'A' && s[i] <= 'F') val |= s[i] - 'A' + 10;
    }
    return val;
}

static void decode_address(kostool_context_t *ctx, uint32_t addr, char *out, size_t out_size) {
    int i;

    /* Check cache */
    for (i = 0; i < addr_cache_count; i++) {
        if (addr_cache[i].addr == addr) {
            strncpy(out, addr_cache[i].decoded, out_size);
            out[out_size - 1] = '\0';
            return;
        }
    }

    /* Run addr2line */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%saddr2line -Cfpe %s 0x%08x",
             ctx->addr2line_prefix ? ctx->addr2line_prefix : "",
             ctx->loaded_binary_path, addr);

    out[0] = '\0';
    FILE *fp = popen(cmd, "r");
    if (fp) {
        if (fgets(out, (int)out_size, fp) == NULL)
            out[0] = '\0';
        size_t len = strlen(out);
        if (len > 0 && out[len - 1] == '\n') out[len - 1] = '\0';
        pclose(fp);
    }

    /* Don't cache useless results like "?? ??:0" */
    if (out[0] == '?' || out[0] == '\0')
        return;

    /* Cache result */
    if (addr_cache_count < ADDR2LINE_CACHE_SIZE) {
        addr_cache[addr_cache_count].addr = addr;
        strncpy(addr_cache[addr_cache_count].decoded, out, 128);
        addr_cache[addr_cache_count].decoded[127] = '\0';
        addr_cache_count++;
    }
}

/*
 * Scan data written to stdout for hex addresses in the program range.
 * For each match, print addr2line-decoded function:line annotation.
 */
static void scan_for_addresses(kostool_context_t *ctx, const uint8_t *data, int count) {
    if (!ctx->addr2line_enabled || !ctx->loaded_binary_path)
        return;

    /* Determine valid address range based on load address */
    uint32_t range_lo, range_hi;
    if (ctx->load_address >= 0x80000000 && ctx->load_address < 0x90000000) {
        /* GC: 0x80100000 - 0x81800000 */
        range_lo = 0x80100000;
        range_hi = 0x81800000;
    } else {
        /* DC: 0x8c010000 - 0x8d000000 */
        range_lo = 0x8c010000;
        range_hi = 0x8d000000;
    }

    const char *s = (const char *)data;
    int i;

    for (i = 0; i < count - 7; i++) {
        int hex_start = i;
        int hex_len = 0;

        /* Skip "0x" prefix if present */
        if (i < count - 9 && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
            hex_start = i + 2;
        }

        /* Count consecutive hex digits */
        while (hex_start + hex_len < count && is_hex_char(s[hex_start + hex_len]))
            hex_len++;

        if (hex_len == 8) {
            uint32_t addr = hex_to_uint(s + hex_start, 8);
            if (addr >= range_lo && addr < range_hi) {
                char decoded[128];
                decode_address(ctx, addr, decoded, sizeof(decoded));
                if (decoded[0] && decoded[0] != '?') {
                    fprintf(stderr, "  -> %s\n", decoded);
                }
            }
            i = hex_start + hex_len - 1;
        }
    }
}

static DIR *opendirs[MAX_OPEN_DIRS];

/* ===== Byte order helpers for target wire format ===== */

/* Convert host uint16 to target byte order.
 * DC (SH4) = little-endian, GC (PPC) = big-endian. */
static uint16_t target_order16(const kostool_context_t *ctx, uint16_t x) {
    if (ctx->target_big_endian)
        return htons(x);
    /* Target is LE: if host is also LE, no-op; if host is BE, swap */
    if (htonl(1) == 1)
        return (uint16_t)((x << 8) | (x >> 8));
    return x;
}

/* Convert host uint32 to target byte order. */
static uint32_t target_order32(const kostool_context_t *ctx, uint32_t x) {
    if (ctx->target_big_endian)
        return htonl(x);
    /* Target is LE: if host is also LE, no-op; if host is BE, swap */
    if (htonl(1) == 1)
        return (x << 24) | ((x << 8) & 0xff0000) |
               ((x >> 8) & 0xff00) | ((x >> 24) & 0xff);
    return x;
}

/* ===== Path resolution ===== */

static const char *resolve_path(kostool_context_t *ctx, const char *path,
                                char *buf, size_t buf_size) {
    return ctx->fs_ops->resolve_path(path, ctx->map_path, buf, buf_size);
}

/* ===== Serial console helpers ===== */

static void ser_blread(kostool_context_t *ctx, void *buf, int count) {
    uint8_t *tmp = buf;
    while (count > 0) {
        int ret = ctx->serial_ops->read(ctx->serial_handle, tmp, count);
        if (ret <= 0) { fprintf(stderr, "blread: read error (%d)\n", ret); return; }
        tmp += ret;
        count -= ret;
    }
}

static void ser_send_uint(kostool_context_t *ctx, uint32_t value) {
    uint8_t b;
    b = value & 0xFF;         ctx->serial_ops->write(ctx->serial_handle, &b, 1);
    b = (value >> 8) & 0xFF;  ctx->serial_ops->write(ctx->serial_handle, &b, 1);
    b = (value >> 16) & 0xFF; ctx->serial_ops->write(ctx->serial_handle, &b, 1);
    b = (value >> 24) & 0xFF; ctx->serial_ops->write(ctx->serial_handle, &b, 1);

    uint8_t e[4];
    ser_blread(ctx, e, 4);
    uint32_t echo = e[0] | ((uint32_t)e[1] << 8) | ((uint32_t)e[2] << 16) | ((uint32_t)e[3] << 24);
    if (echo != value)
        fprintf(stderr, "send_uint: echo mismatch (sent 0x%08x, got 0x%08x)\n", value, echo);
}

static uint32_t ser_recv_uint(kostool_context_t *ctx) {
    uint8_t b[4];
    ser_blread(ctx, b, 4);
    return b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/* Raw LZO send for serial syscalls (no command prefix) */
static void ser_send_data(kostool_context_t *ctx, const uint8_t *addr, uint32_t size) {
    static long __LZO_MMODEL wrkmem[((LZO1X_1_MEM_COMPRESS) + (sizeof(long) - 1)) / sizeof(long)];
    uint8_t *buffer = malloc(SERIAL_BUFFER_SIZE + SERIAL_BUFFER_SIZE / 64 + 16 + 3);
    if (!buffer) return;

    while (size > 0) {
        uint32_t sendsize = (size > SERIAL_BUFFER_SIZE) ? SERIAL_BUFFER_SIZE : size;
        lzo_uint csize;
        lzo1x_1_compress(addr, sendsize, buffer, &csize, wrkmem);

        if (csize < sendsize) {
            uint8_t c = SERIAL_DATA_COMPRESSED;
            ctx->serial_ops->write(ctx->serial_handle, &c, 1);
            ser_send_uint(ctx, csize);
            uint8_t ack = SERIAL_DATA_BAD;
            while (ack != SERIAL_DATA_GOOD) {
                ctx->serial_ops->write(ctx->serial_handle, buffer, csize);
                uint8_t sum = 0;
                for (lzo_uint i = 0; i < csize; i++) sum ^= buffer[i];
                ctx->serial_ops->write(ctx->serial_handle, &sum, 1);
                ser_blread(ctx, &ack, 1);
            }
        } else {
            uint8_t c = SERIAL_DATA_UNCOMPRESSED;
            ctx->serial_ops->write(ctx->serial_handle, &c, 1);
            ser_send_uint(ctx, sendsize);
            ctx->serial_ops->write(ctx->serial_handle, addr, sendsize);
            uint8_t sum = 0;
            for (uint32_t i = 0; i < sendsize; i++) sum ^= addr[i];
            ctx->serial_ops->write(ctx->serial_handle, &sum, 1);
            uint8_t ack;
            ser_blread(ctx, &ack, 1);
        }
        size -= sendsize;
        addr += sendsize;
    }
    free(buffer);
}

/* Raw LZO recv for serial syscalls (no command prefix) */
static void ser_recv_data(kostool_context_t *ctx, void *data, uint32_t total) {
    uint8_t *out = data;
    while (total > 0) {
        uint8_t type;
        ser_blread(ctx, &type, 1);
        uint32_t size = ser_recv_uint(ctx);

        if (type == SERIAL_DATA_UNCOMPRESSED) {
            ser_blread(ctx, out, size);
            uint8_t sum;
            ser_blread(ctx, &sum, 1);
            uint8_t ok = SERIAL_DATA_GOOD;
            ctx->serial_ops->write(ctx->serial_handle, &ok, 1);
            total -= size;
            out += size;
        } else if (type == SERIAL_DATA_COMPRESSED) {
            uint8_t *tmp = malloc(size);
            if (!tmp) {
                fprintf(stderr, "\nser_recv_data: malloc(%u) failed\n", size);
                return;
            }
            ser_blread(ctx, tmp, size);
            uint8_t sum;
            ser_blread(ctx, &sum, 1);
            lzo_uint newsize;
            if (lzo1x_decompress(tmp, size, out, &newsize, 0) == LZO_E_OK) {
                uint8_t ok = SERIAL_DATA_GOOD;
                ctx->serial_ops->write(ctx->serial_handle, &ok, 1);
                total -= newsize;
                out += newsize;
            } else {
                uint8_t bad = SERIAL_DATA_BAD;
                ctx->serial_ops->write(ctx->serial_handle, &bad, 1);
                fprintf(stderr, "\nrecv_data: decompression failed!\n");
            }
            free(tmp);
        }
    }
}

/* ===== Network console helpers ===== */

static int net_send_cmd(kostool_context_t *ctx, const char cmd[4],
                        uint32_t addr, uint32_t size,
                        const uint8_t *data, uint32_t dsize) {
    uint8_t buf[2048];
    uint32_t tmp;
    if (dsize > sizeof(buf) - 12)
        dsize = sizeof(buf) - 12;
    memcpy(buf, cmd, 4);
    tmp = htonl(addr); memcpy(buf + 4, &tmp, 4);
    tmp = htonl(size); memcpy(buf + 8, &tmp, 4);
    if (data && dsize > 0) memcpy(buf + 12, data, dsize);
    return ctx->socket_ops->send(ctx->global_socket, buf, 12 + dsize);
}

static int net_recv_resp(kostool_context_t *ctx, uint8_t *buffer,
                         size_t buffer_size, int timeout) {
    uint64_t start = ctx->time_ops->time_usec();
    while ((ctx->time_ops->time_usec() - start) < (uint64_t)timeout) {
        int rv = ctx->socket_ops->recv(ctx->global_socket, buffer, buffer_size);
        if (rv > 0) return rv;
    }
    return -1;
}

/* ===== Serial console syscall handlers ===== */

static void ser_dc_fstat(kostool_context_t *ctx) {
    int fd = ser_recv_uint(ctx);
    struct stat st = {0};
    int ret = fstat(fd, &st);
    ser_send_uint(ctx, st.st_dev);
    ser_send_uint(ctx, st.st_ino);
    ser_send_uint(ctx, st.st_mode);
    ser_send_uint(ctx, st.st_nlink);
    ser_send_uint(ctx, st.st_uid);
    ser_send_uint(ctx, st.st_gid);
    ser_send_uint(ctx, st.st_rdev);
    ser_send_uint(ctx, st.st_size);
#ifdef _WIN32
    ser_send_uint(ctx, 0);
    ser_send_uint(ctx, 0);
#else
    ser_send_uint(ctx, st.st_blksize);
    ser_send_uint(ctx, st.st_blocks);
#endif
    ser_send_uint(ctx, st.st_atime);
    ser_send_uint(ctx, st.st_mtime);
    ser_send_uint(ctx, st.st_ctime);
    ser_send_uint(ctx, ret);
}

static void ser_dc_write(kostool_context_t *ctx) {
    int fd = ser_recv_uint(ctx);
    uint32_t count = ser_recv_uint(ctx);
    if (!count || count > MAX_SYSCALL_SIZE) { ser_send_uint(ctx, -1); return; }
    uint8_t *data = malloc(count);
    if (!data) { ser_send_uint(ctx, -1); return; }
    ser_recv_data(ctx, data, count);
    int ret = write(fd, data, count);
    if (fd == 1)
        scan_for_addresses(ctx, data, count);
    ser_send_uint(ctx, ret);
    free(data);
}

static void ser_dc_read(kostool_context_t *ctx) {
    int fd = ser_recv_uint(ctx);
    uint32_t count = ser_recv_uint(ctx);
    if (!count || count > MAX_SYSCALL_SIZE) { ser_send_uint(ctx, -1); return; }
    uint8_t *data = malloc(count);
    if (!data) { ser_send_uint(ctx, -1); return; }
    int ret = read(fd, data, count);
    ser_send_data(ctx, data, count);
    ser_send_uint(ctx, ret);
    free(data);
}

static void ser_dc_open(kostool_context_t *ctx) {
    uint32_t namelen = ser_recv_uint(ctx);
    if (!namelen || namelen > MAX_PATH_LEN) { ser_send_uint(ctx, -1); return; }
    char *pathname = malloc(namelen);
    if (!pathname) { ser_send_uint(ctx, -1); return; }
    ser_recv_data(ctx, pathname, namelen);
    int flags = ser_recv_uint(ctx);
    int mode = ser_recv_uint(ctx);

    int ourflags = ctx->fs_ops->translate_open_flags(flags);
    char buf[MAX_PATH_LEN];
    const char *resolved = resolve_path(ctx, pathname, buf, sizeof(buf));
    int ret = open(resolved, ourflags | O_BINARY, mode);
    ser_send_uint(ctx, ret);
    free(pathname);
}

static void ser_dc_close(kostool_context_t *ctx) {
    int fd = ser_recv_uint(ctx);
    int ret = close(fd);
    ser_send_uint(ctx, ret);
}

static void ser_dc_creat(kostool_context_t *ctx) {
    uint32_t namelen = ser_recv_uint(ctx);
    if (!namelen || namelen > MAX_PATH_LEN) { ser_send_uint(ctx, -1); return; }
    char *pathname = malloc(namelen);
    if (!pathname) { ser_send_uint(ctx, -1); return; }
    ser_recv_data(ctx, pathname, namelen);
    int mode = ser_recv_uint(ctx);
    int ret = creat(pathname, mode);
    ser_send_uint(ctx, ret);
    free(pathname);
}

static void ser_dc_link(kostool_context_t *ctx) {
    uint32_t len1 = ser_recv_uint(ctx);
    if (!len1 || len1 > MAX_PATH_LEN) { ser_send_uint(ctx, -1); return; }
    char *path1 = malloc(len1);
    if (!path1) { ser_send_uint(ctx, -1); return; }
    ser_recv_data(ctx, path1, len1);
    uint32_t len2 = ser_recv_uint(ctx);
    if (!len2 || len2 > MAX_PATH_LEN) { free(path1); ser_send_uint(ctx, -1); return; }
    char *path2 = malloc(len2);
    if (!path2) { free(path1); ser_send_uint(ctx, -1); return; }
    ser_recv_data(ctx, path2, len2);
    int ret = link(path1, path2);
    ser_send_uint(ctx, ret);
    free(path1); free(path2);
}

static void ser_dc_unlink(kostool_context_t *ctx) {
    uint32_t namelen = ser_recv_uint(ctx);
    if (!namelen || namelen > MAX_PATH_LEN) { ser_send_uint(ctx, -1); return; }
    char *pathname = malloc(namelen);
    if (!pathname) { ser_send_uint(ctx, -1); return; }
    ser_recv_data(ctx, pathname, namelen);
    int ret = unlink(pathname);
    ser_send_uint(ctx, ret);
    free(pathname);
}

static void ser_dc_chdir(kostool_context_t *ctx) {
    uint32_t namelen = ser_recv_uint(ctx);
    if (!namelen || namelen > MAX_PATH_LEN) { ser_send_uint(ctx, -1); return; }
    char *pathname = malloc(namelen);
    if (!pathname) { ser_send_uint(ctx, -1); return; }
    ser_recv_data(ctx, pathname, namelen);
    char buf[MAX_PATH_LEN];
    const char *resolved = resolve_path(ctx, pathname, buf, sizeof(buf));
    int ret = chdir(resolved);
    ser_send_uint(ctx, ret);
    free(pathname);
}

static void ser_dc_chmod(kostool_context_t *ctx) {
    uint32_t namelen = ser_recv_uint(ctx);
    if (!namelen || namelen > MAX_PATH_LEN) { ser_send_uint(ctx, -1); return; }
    char *pathname = malloc(namelen);
    if (!pathname) { ser_send_uint(ctx, -1); return; }
    ser_recv_data(ctx, pathname, namelen);
    int mode = ser_recv_uint(ctx);
    int ret = chmod(pathname, mode);
    ser_send_uint(ctx, ret);
    free(pathname);
}

static void ser_dc_mkdir(kostool_context_t *ctx) {
    uint32_t namelen = ser_recv_uint(ctx);
    if (!namelen || namelen > MAX_PATH_LEN) { ser_send_uint(ctx, -1); return; }
    char *pathname = malloc(namelen);
    if (!pathname) { ser_send_uint(ctx, -1); return; }
    ser_recv_data(ctx, pathname, namelen);
    int mode = ser_recv_uint(ctx);
    char buf[MAX_PATH_LEN];
    const char *resolved = resolve_path(ctx, pathname, buf, sizeof(buf));
    int ret = mkdir(resolved, mode);
    ser_send_uint(ctx, ret);
    free(pathname);
}

static void ser_dc_lseek(kostool_context_t *ctx) {
    int fd = ser_recv_uint(ctx);
    int offset = ser_recv_uint(ctx);
    int whence = ser_recv_uint(ctx);
    int ret = lseek(fd, offset, whence);
    ser_send_uint(ctx, ret);
}

static void ser_dc_time(kostool_context_t *ctx) {
    time_t t;
    time(&t);
    ser_send_uint(ctx, (uint32_t)t);
}

static void ser_dc_stat(kostool_context_t *ctx) {
    uint32_t namelen = ser_recv_uint(ctx);
    if (!namelen || namelen > MAX_PATH_LEN) { ser_send_uint(ctx, -1); return; }
    char *filename = malloc(namelen);
    if (!filename) { ser_send_uint(ctx, -1); return; }
    ser_recv_data(ctx, filename, namelen);
    struct stat st = {0};
    char buf[MAX_PATH_LEN];
    const char *resolved = resolve_path(ctx, filename, buf, sizeof(buf));
    int ret = stat(resolved, &st);
    ser_send_uint(ctx, st.st_dev);
    ser_send_uint(ctx, st.st_ino);
    ser_send_uint(ctx, st.st_mode);
    ser_send_uint(ctx, st.st_nlink);
    ser_send_uint(ctx, st.st_uid);
    ser_send_uint(ctx, st.st_gid);
    ser_send_uint(ctx, st.st_rdev);
    ser_send_uint(ctx, st.st_size);
#ifdef _WIN32
    ser_send_uint(ctx, 0);
    ser_send_uint(ctx, 0);
#else
    ser_send_uint(ctx, st.st_blksize);
    ser_send_uint(ctx, st.st_blocks);
#endif
    ser_send_uint(ctx, st.st_atime);
    ser_send_uint(ctx, st.st_mtime);
    ser_send_uint(ctx, st.st_ctime);
    ser_send_uint(ctx, ret);
    free(filename);
}

static void ser_dc_utime(kostool_context_t *ctx) {
    uint32_t namelen = ser_recv_uint(ctx);
    if (!namelen || namelen > MAX_PATH_LEN) { ser_send_uint(ctx, -1); return; }
    char *pathname = malloc(namelen);
    if (!pathname) { ser_send_uint(ctx, -1); return; }
    ser_recv_data(ctx, pathname, namelen);
    int has_times = ser_recv_uint(ctx);
    int ret;
    if (has_times) {
        struct utimbuf tbuf;
        tbuf.actime = ser_recv_uint(ctx);
        tbuf.modtime = ser_recv_uint(ctx);
        ret = utime(pathname, &tbuf);
    } else {
        ret = utime(pathname, NULL);
    }
    ser_send_uint(ctx, ret);
    free(pathname);
}

static void ser_dc_opendir(kostool_context_t *ctx) {
    uint32_t namelen = ser_recv_uint(ctx);
    if (!namelen || namelen > MAX_PATH_LEN) { ser_send_uint(ctx, 0); return; }
    char *dirname_str = malloc(namelen);
    if (!dirname_str) { ser_send_uint(ctx, 0); return; }
    ser_recv_data(ctx, dirname_str, namelen);

    uint32_t i;
    for (i = 0; i < MAX_OPEN_DIRS; i++)
        if (!opendirs[i]) break;

    if (i < MAX_OPEN_DIRS) {
        char buf[MAX_PATH_LEN];
        const char *resolved = resolve_path(ctx, dirname_str, buf, sizeof(buf));
        if (!(opendirs[i] = opendir(resolved)))
            i = 0;
        else
            i += DIRENT_OFFSET;
    } else {
        i = 0;
    }
    ser_send_uint(ctx, i);
    free(dirname_str);
}

static void ser_dc_closedir(kostool_context_t *ctx) {
    uint32_t i = ser_recv_uint(ctx);
    int ret;
    if (i >= DIRENT_OFFSET && i < MAX_OPEN_DIRS + DIRENT_OFFSET) {
        ret = closedir(opendirs[i - DIRENT_OFFSET]);
        opendirs[i - DIRENT_OFFSET] = NULL;
    } else {
        ret = -1;
    }
    ser_send_uint(ctx, ret);
}

static void ser_dc_readdir(kostool_context_t *ctx) {
    uint32_t i = ser_recv_uint(ctx);
    struct dirent *de = NULL;
    if (i >= DIRENT_OFFSET && i < MAX_OPEN_DIRS + DIRENT_OFFSET)
        de = readdir(opendirs[i - DIRENT_OFFSET]);

    if (!de) { ser_send_uint(ctx, 0); return; }
    ser_send_uint(ctx, 1);
    ser_send_uint(ctx, de->d_ino);
#if defined(__APPLE__) || defined(__FreeBSD__)
    ser_send_uint(ctx, 0); /* d_off not available */
#elif !defined(_WIN32) && !defined(__CYGWIN__)
    ser_send_uint(ctx, de->d_off);
#else
    ser_send_uint(ctx, 0);
#endif
#if !defined(_WIN32) && !defined(__CYGWIN__)
    ser_send_uint(ctx, de->d_reclen);
    ser_send_uint(ctx, de->d_type);
#else
    ser_send_uint(ctx, 0);
    ser_send_uint(ctx, 0);
#endif
    uint32_t namelen = strlen(de->d_name) + 1;
    if (namelen > 256) namelen = 256;
    ser_send_uint(ctx, namelen);
    ser_send_data(ctx, (const uint8_t *)de->d_name, namelen);
}

static void ser_dc_rewinddir(kostool_context_t *ctx) {
    uint32_t i = ser_recv_uint(ctx);
    if (i >= DIRENT_OFFSET && i < MAX_OPEN_DIRS + DIRENT_OFFSET) {
        rewinddir(opendirs[i - DIRENT_OFFSET]);
    }
    ser_send_uint(ctx, 0);
}

static void ser_dc_cdfs_read(kostool_context_t *ctx) {
    int start = ser_recv_uint(ctx);
    uint32_t num = ser_recv_uint(ctx);
    start -= 150;
    if (!num || num > MAX_SYSCALL_SIZE / 2048) { ser_send_uint(ctx, -1); return; }
    uint32_t bytes = num * 2048;
    if (bytes > MAX_SYSCALL_SIZE) { ser_send_uint(ctx, -1); return; }
    uint8_t *buf = malloc(bytes);
    if (!buf) { ser_send_uint(ctx, -1); return; }
    memset(buf, 0, bytes);
    if (ctx->cdfs_fd >= 0) {
        if (lseek(ctx->cdfs_fd, (off_t)start * 2048, SEEK_SET) != (off_t)-1) {
            ssize_t rd = read(ctx->cdfs_fd, buf, bytes);
            if (rd < 0) rd = 0;
            /* Zero-fill any remainder if short read */
            if ((uint32_t)rd < bytes)
                memset(buf + rd, 0, bytes - rd);
        }
    }
    ser_send_data(ctx, buf, bytes);
    free(buf);
}

static void ser_dc_gdbpacket(kostool_context_t *ctx) {
    uint32_t in_size = ser_recv_uint(ctx);
    uint32_t out_size = ser_recv_uint(ctx);
    static char gdb_buf[1024];
    int retval = 0;

    if (in_size)
        ser_recv_data(ctx, gdb_buf, in_size > 1024 ? 1024 : in_size);

    if (ctx->gdb_server_socket < 0) {
        ser_send_uint(ctx, (uint32_t)-1);
        return;
    }

    if (ctx->gdb_client_socket <= 0) {
        printf("waiting for gdb client connection...\n");
        ctx->gdb_client_socket = ctx->socket_ops->accept(ctx->gdb_server_socket);
        if (ctx->gdb_client_socket <= 0) {
            fprintf(stderr, "error accepting gdb connection\n");
            ser_send_uint(ctx, (uint32_t)-1);
            return;
        }
        printf("GDB client connected\n");
    }

    if (in_size)
        ctx->socket_ops->send(ctx->gdb_client_socket, gdb_buf, in_size);

    if (out_size) {
        retval = ctx->socket_ops->recv(ctx->gdb_client_socket,
                                       gdb_buf, out_size > 1024 ? 1024 : out_size);
        if (retval == 0) {
            printf("GDB client disconnected\n");
            ctx->socket_ops->close(ctx->gdb_client_socket);
            ctx->gdb_client_socket = -1;
        }
    }
    if (retval < 0) {
        fprintf(stderr, "GDB socket error\n");
        return;
    }
    ser_send_uint(ctx, retval);
    if (retval > 0)
        ser_send_data(ctx, (const uint8_t *)gdb_buf, retval);
}

static int ser_dc_exit(kostool_context_t *ctx) {
    return (int32_t)ser_recv_uint(ctx);
}

/* ===== Network console syscall handlers ===== */

static void net_dc_fstat(kostool_context_t *ctx, uint8_t *pkt) {
    net_command_3int_t *cmd = (net_command_3int_t *)pkt;
    int fd = ntohl(cmd->value0);
    uint32_t addr = ntohl(cmd->value1);
    uint32_t sz = ntohl(cmd->value2);
    struct stat st = {0};
    int ret = fstat(fd, &st);
    dcload_stat_t ds = {0};
    ds.st_dev = target_order16(ctx, st.st_dev);
    ds.st_ino = target_order16(ctx, st.st_ino);
    ds.st_mode = target_order32(ctx, st.st_mode);
    ds.st_nlink = target_order16(ctx, st.st_nlink);
    ds.st_uid = target_order16(ctx, st.st_uid);
    ds.st_gid = target_order16(ctx, st.st_gid);
    ds.st_rdev = target_order16(ctx, st.st_rdev);
    ds.st_size = target_order32(ctx, st.st_size);
#ifndef _WIN32
    ds.st_blksize = target_order32(ctx, st.st_blksize);
    ds.st_blocks = target_order32(ctx, st.st_blocks);
#endif
    ds.st_atime_val = target_order32(ctx, st.st_atime);
    ds.st_mtime_val = target_order32(ctx, st.st_mtime);
    ds.st_ctime_val = target_order32(ctx, st.st_ctime);
    ctx->transport->send_data(ctx, (uint8_t *)&ds, addr, sz);
    net_send_cmd(ctx, NET_CMD_RETVAL, ret, ret, NULL, 0);
}

static void net_dc_write(kostool_context_t *ctx, uint8_t *pkt) {
    net_command_3int_t *cmd = (net_command_3int_t *)pkt;
    int fd = ntohl(cmd->value0);
    uint32_t addr = ntohl(cmd->value1);
    uint32_t count = ntohl(cmd->value2);
    if (!count || count > MAX_SYSCALL_SIZE) {
        net_send_cmd(ctx, NET_CMD_RETVAL, (uint32_t)-1, (uint32_t)-1, NULL, 0);
        return;
    }
    uint8_t *data = malloc(count);
    if (!data) {
        net_send_cmd(ctx, NET_CMD_RETVAL, (uint32_t)-1, (uint32_t)-1, NULL, 0);
        return;
    }
    ctx->transport->recv_data(ctx, data, addr, count, 1);
    int ret = write(fd, data, count);
    if (fd == 1)
        scan_for_addresses(ctx, data, count);
    net_send_cmd(ctx, NET_CMD_RETVAL, ret, ret, NULL, 0);
    free(data);
}

static void net_dc_read(kostool_context_t *ctx, uint8_t *pkt) {
    net_command_3int_t *cmd = (net_command_3int_t *)pkt;
    int fd = ntohl(cmd->value0);
    uint32_t addr = ntohl(cmd->value1);
    uint32_t count = ntohl(cmd->value2);
    if (!count || count > MAX_SYSCALL_SIZE) {
        net_send_cmd(ctx, NET_CMD_RETVAL, (uint32_t)-1, (uint32_t)-1, NULL, 0);
        return;
    }
    uint8_t *data = malloc(count);
    if (!data) {
        net_send_cmd(ctx, NET_CMD_RETVAL, (uint32_t)-1, (uint32_t)-1, NULL, 0);
        return;
    }
    int ret = read(fd, data, count);
    ctx->transport->send_data(ctx, data, addr, count);
    net_send_cmd(ctx, NET_CMD_RETVAL, ret, ret, NULL, 0);
    free(data);
}

static void net_dc_open(kostool_context_t *ctx, uint8_t *pkt) {
    net_command_2int_string_t *cmd = (net_command_2int_string_t *)pkt;
    int flags = ntohl(cmd->value0);
    int mode = ntohl(cmd->value1);
    int ourflags = ctx->fs_ops->translate_open_flags(flags);
    char buf[MAX_PATH_LEN];
    const char *resolved = resolve_path(ctx, cmd->string, buf, sizeof(buf));
    int ret = open(resolved, ourflags | O_BINARY, mode);
    net_send_cmd(ctx, NET_CMD_RETVAL, ret, ret, NULL, 0);
}

static void net_dc_close(kostool_context_t *ctx, uint8_t *pkt) {
    net_command_int_t *cmd = (net_command_int_t *)pkt;
    int ret = close(ntohl(cmd->value0));
    net_send_cmd(ctx, NET_CMD_RETVAL, ret, ret, NULL, 0);
}

static void net_dc_creat(kostool_context_t *ctx, uint8_t *pkt) {
    net_command_int_string_t *cmd = (net_command_int_string_t *)pkt;
    char buf[MAX_PATH_LEN];
    const char *resolved = resolve_path(ctx, cmd->string, buf, sizeof(buf));
    int ret = creat(resolved, ntohl(cmd->value0));
    net_send_cmd(ctx, NET_CMD_RETVAL, ret, ret, NULL, 0);
}

static void net_dc_link(kostool_context_t *ctx, uint8_t *pkt) {
    net_command_string_t *cmd = (net_command_string_t *)pkt;
    const char *path1 = cmd->string;
    const char *path2 = path1 + strlen(path1) + 1;
    int ret = link(path1, path2);
    net_send_cmd(ctx, NET_CMD_RETVAL, ret, ret, NULL, 0);
}

static void net_dc_unlink(kostool_context_t *ctx, uint8_t *pkt) {
    net_command_string_t *cmd = (net_command_string_t *)pkt;
    int ret = unlink(cmd->string);
    net_send_cmd(ctx, NET_CMD_RETVAL, ret, ret, NULL, 0);
}

static void net_dc_chdir(kostool_context_t *ctx, uint8_t *pkt) {
    net_command_string_t *cmd = (net_command_string_t *)pkt;
    char buf[MAX_PATH_LEN];
    const char *resolved = resolve_path(ctx, cmd->string, buf, sizeof(buf));
    int ret = chdir(resolved);
    net_send_cmd(ctx, NET_CMD_RETVAL, ret, ret, NULL, 0);
}

static void net_dc_chmod(kostool_context_t *ctx, uint8_t *pkt) {
    net_command_int_string_t *cmd = (net_command_int_string_t *)pkt;
    char buf[MAX_PATH_LEN];
    const char *resolved = resolve_path(ctx, cmd->string, buf, sizeof(buf));
    int ret = chmod(resolved, ntohl(cmd->value0));
    net_send_cmd(ctx, NET_CMD_RETVAL, ret, ret, NULL, 0);
}

static void net_dc_mkdir(kostool_context_t *ctx, uint8_t *pkt) {
    net_command_int_string_t *cmd = (net_command_int_string_t *)pkt;
    char buf[MAX_PATH_LEN];
    const char *resolved = resolve_path(ctx, cmd->string, buf, sizeof(buf));
    int ret = mkdir(resolved, ntohl(cmd->value0));
    net_send_cmd(ctx, NET_CMD_RETVAL, ret, ret, NULL, 0);
}

static void net_dc_lseek(kostool_context_t *ctx, uint8_t *pkt) {
    net_command_3int_t *cmd = (net_command_3int_t *)pkt;
    int ret = lseek(ntohl(cmd->value0), ntohl(cmd->value1), ntohl(cmd->value2));
    net_send_cmd(ctx, NET_CMD_RETVAL, ret, ret, NULL, 0);
}

static void net_dc_time(kostool_context_t *ctx, uint8_t *pkt) {
    (void)pkt;
    time_t t = time(NULL);
    net_send_cmd(ctx, NET_CMD_RETVAL, (uint32_t)t, (uint32_t)t, NULL, 0);
}

static void net_dc_stat(kostool_context_t *ctx, uint8_t *pkt) {
    net_command_2int_string_t *cmd = (net_command_2int_string_t *)pkt;
    uint32_t addr = ntohl(cmd->value0);
    uint32_t sz = ntohl(cmd->value1);
    struct stat st = {0};
    char buf[MAX_PATH_LEN];
    const char *resolved = resolve_path(ctx, cmd->string, buf, sizeof(buf));
    int ret = stat(resolved, &st);
    dcload_stat_t ds = {0};
    ds.st_dev = target_order16(ctx, st.st_dev);
    ds.st_ino = target_order16(ctx, st.st_ino);
    ds.st_mode = target_order32(ctx, st.st_mode);
    ds.st_nlink = target_order16(ctx, st.st_nlink);
    ds.st_uid = target_order16(ctx, st.st_uid);
    ds.st_gid = target_order16(ctx, st.st_gid);
    ds.st_rdev = target_order16(ctx, st.st_rdev);
    ds.st_size = target_order32(ctx, st.st_size);
#ifndef _WIN32
    ds.st_blksize = target_order32(ctx, st.st_blksize);
    ds.st_blocks = target_order32(ctx, st.st_blocks);
#endif
    ds.st_atime_val = target_order32(ctx, st.st_atime);
    ds.st_mtime_val = target_order32(ctx, st.st_mtime);
    ds.st_ctime_val = target_order32(ctx, st.st_ctime);
    ctx->transport->send_data(ctx, (uint8_t *)&ds, addr, sz);
    net_send_cmd(ctx, NET_CMD_RETVAL, ret, ret, NULL, 0);
}

static void net_dc_utime(kostool_context_t *ctx, uint8_t *pkt) {
    net_command_3int_string_t *cmd = (net_command_3int_string_t *)pkt;
    int ret;
    if (ntohl(cmd->value0)) {
        struct utimbuf tbuf;
        tbuf.actime = ntohl(cmd->value1);
        tbuf.modtime = ntohl(cmd->value2);
        ret = utime(cmd->string, &tbuf);
    } else {
        ret = utime(cmd->string, NULL);
    }
    net_send_cmd(ctx, NET_CMD_RETVAL, ret, ret, NULL, 0);
}

static void net_dc_opendir(kostool_context_t *ctx, uint8_t *pkt) {
    net_command_string_t *cmd = (net_command_string_t *)pkt;
    uint32_t i;
    for (i = 0; i < MAX_OPEN_DIRS; i++)
        if (!opendirs[i]) break;
    if (i < MAX_OPEN_DIRS) {
        char buf[MAX_PATH_LEN];
        const char *resolved = resolve_path(ctx, cmd->string, buf, sizeof(buf));
        if (!(opendirs[i] = opendir(resolved)))
            i = 0;
        else
            i += DIRENT_OFFSET;
    } else {
        i = 0;
    }
    net_send_cmd(ctx, NET_CMD_RETVAL, i, i, NULL, 0);
}

static void net_dc_closedir(kostool_context_t *ctx, uint8_t *pkt) {
    net_command_int_t *cmd = (net_command_int_t *)pkt;
    uint32_t i = ntohl(cmd->value0);
    int ret;
    if (i >= DIRENT_OFFSET && i < MAX_OPEN_DIRS + DIRENT_OFFSET) {
        ret = closedir(opendirs[i - DIRENT_OFFSET]);
        opendirs[i - DIRENT_OFFSET] = NULL;
    } else {
        ret = -1;
    }
    net_send_cmd(ctx, NET_CMD_RETVAL, ret, ret, NULL, 0);
}

static void net_dc_readdir(kostool_context_t *ctx, uint8_t *pkt) {
    net_command_3int_t *cmd = (net_command_3int_t *)pkt;
    uint32_t i = ntohl(cmd->value0);
    uint32_t addr = ntohl(cmd->value1);
    uint32_t sz = ntohl(cmd->value2);
    struct dirent *de = NULL;
    if (i >= DIRENT_OFFSET && i < MAX_OPEN_DIRS + DIRENT_OFFSET)
        de = readdir(opendirs[i - DIRENT_OFFSET]);
    if (de) {
        dcload_dirent_t dd = {0};
        dd.d_ino = target_order32(ctx, de->d_ino);
#if defined(__APPLE__) || defined(__FreeBSD__)
        dd.d_off = 0;
        dd.d_reclen = target_order32(ctx, de->d_reclen);
        dd.d_type = de->d_type;
#elif defined(_WIN32) || defined(__CYGWIN__)
        dd.d_off = 0;
        dd.d_reclen = 0;
        dd.d_type = 0;
#else
        dd.d_off = target_order32(ctx, de->d_off);
        dd.d_reclen = target_order32(ctx, de->d_reclen);
        dd.d_type = de->d_type;
#endif
        strncpy(dd.d_name, de->d_name, sizeof(dd.d_name) - 1);
        ctx->transport->send_data(ctx, (uint8_t *)&dd, addr, sz);
        net_send_cmd(ctx, NET_CMD_RETVAL, 1, 1, NULL, 0);
    } else {
        net_send_cmd(ctx, NET_CMD_RETVAL, 0, 0, NULL, 0);
    }
}

static void net_dc_rewinddir(kostool_context_t *ctx, uint8_t *pkt) {
    net_command_int_t *cmd = (net_command_int_t *)pkt;
    uint32_t i = ntohl(cmd->value0);
    int ret;
    if (i >= DIRENT_OFFSET && i < MAX_OPEN_DIRS + DIRENT_OFFSET) {
        rewinddir(opendirs[i - DIRENT_OFFSET]);
        ret = 0;
    } else {
        ret = -1;
    }
    net_send_cmd(ctx, NET_CMD_RETVAL, ret, ret, NULL, 0);
}

static void net_dc_cdfs_read(kostool_context_t *ctx, uint8_t *pkt) {
    net_command_3int_t *cmd = (net_command_3int_t *)pkt;
    int start = ntohl(cmd->value0) - 150;
    uint32_t addr = ntohl(cmd->value1);
    uint32_t bytes = ntohl(cmd->value2);
    if (!bytes || bytes > MAX_SYSCALL_SIZE) {
        net_send_cmd(ctx, NET_CMD_RETVAL, (uint32_t)-1, (uint32_t)-1, NULL, 0);
        return;
    }
    uint8_t *buf = malloc(bytes);
    if (!buf) {
        net_send_cmd(ctx, NET_CMD_RETVAL, (uint32_t)-1, (uint32_t)-1, NULL, 0);
        return;
    }
    memset(buf, 0, bytes);
    if (ctx->cdfs_fd >= 0) {
        if (lseek(ctx->cdfs_fd, (off_t)start * 2048, SEEK_SET) != (off_t)-1) {
            ssize_t rd = read(ctx->cdfs_fd, buf, bytes);
            if (rd < 0) rd = 0;
            /* Zero-fill any remainder if short read */
            if ((uint32_t)rd < bytes)
                memset(buf + rd, 0, bytes - rd);
        }
    }
    ctx->transport->send_data(ctx, buf, addr, bytes);
    net_send_cmd(ctx, NET_CMD_RETVAL, 0, 0, NULL, 0);
    free(buf);
}

static void net_dc_gdbpacket(kostool_context_t *ctx, uint8_t *pkt) {
    net_command_2int_string_t *cmd = (net_command_2int_string_t *)pkt;
    uint32_t in_size = ntohl(cmd->value0);
    uint32_t out_size = ntohl(cmd->value1);
    static char gdb_buf[1024];
    int retval = 0;

    if (ctx->gdb_server_socket < 0) {
        net_send_cmd(ctx, NET_CMD_RETVAL, (uint32_t)-1, (uint32_t)-1, NULL, 0);
        return;
    }

    if (ctx->gdb_client_socket <= 0) {
        printf("waiting for gdb client connection...\n");
        ctx->gdb_client_socket = ctx->socket_ops->accept(ctx->gdb_server_socket);
        if (ctx->gdb_client_socket <= 0) {
            fprintf(stderr, "error accepting gdb connection\n");
            net_send_cmd(ctx, NET_CMD_RETVAL, (uint32_t)-1, (uint32_t)-1, NULL, 0);
            return;
        }
        printf("GDB client connected\n");
    }

    if (in_size)
        ctx->socket_ops->send(ctx->gdb_client_socket, cmd->string, in_size);

    if (out_size) {
        retval = ctx->socket_ops->recv(ctx->gdb_client_socket,
                                       gdb_buf, out_size > 1024 ? 1024 : out_size);
        if (retval == 0) {
            printf("GDB client disconnected\n");
            ctx->socket_ops->close(ctx->gdb_client_socket);
            ctx->gdb_client_socket = -1;
        }
    }
    if (retval < 0) retval = 0;
    net_send_cmd(ctx, NET_CMD_RETVAL, retval, retval,
                 (const uint8_t *)gdb_buf, retval);
}

/* ===== Dumb terminal mode ===== */

static void do_dumbterm(kostool_context_t *ctx) {
    printf("\nDumb terminal mode\n\n");
    fflush(stdout);
    while (1) {
        uint8_t c;
        ser_blread(ctx, &c, 1);
        printf("%c", c);
        fflush(stdout);
    }
}

/* ===== Serial console loop ===== */

static int do_serial_console(kostool_context_t *ctx) {
    if (ctx->cdfs_enabled && ctx->iso_filename) {
        ctx->cdfs_fd = open(ctx->iso_filename, O_RDONLY | O_BINARY);
        if (ctx->cdfs_fd < 0)
            perror(ctx->iso_filename);
    }

    if (ctx->use_chroot && ctx->chroot_path) {
        ctx->fs_ops->chroot(ctx->chroot_path);
    }

    while (1) {
        fflush(stdout);
        uint8_t command;
        ser_blread(ctx, &command, 1);

        switch (command) {
        case SERIAL_SYSCALL_EXIT:
            /* Byte 0 is not sent by any firmware function — it indicates
             * a serial line glitch (break condition, SCIF reinitialization
             * by the loaded program, or baud rate mismatch). Ignore it. */
            break;
        case SERIAL_SYSCALL_FSTAT:     ser_dc_fstat(ctx); break;
        case SERIAL_SYSCALL_WRITE:     ser_dc_write(ctx); break;
        case SERIAL_SYSCALL_READ:      ser_dc_read(ctx); break;
        case SERIAL_SYSCALL_OPEN:      ser_dc_open(ctx); break;
        case SERIAL_SYSCALL_CLOSE:     ser_dc_close(ctx); break;
        case SERIAL_SYSCALL_CREAT:     ser_dc_creat(ctx); break;
        case SERIAL_SYSCALL_LINK:      ser_dc_link(ctx); break;
        case SERIAL_SYSCALL_UNLINK:    ser_dc_unlink(ctx); break;
        case SERIAL_SYSCALL_CHDIR:     ser_dc_chdir(ctx); break;
        case SERIAL_SYSCALL_CHMOD:     ser_dc_chmod(ctx); break;
        case SERIAL_SYSCALL_LSEEK:     ser_dc_lseek(ctx); break;
        case SERIAL_SYSCALL_TIME:      ser_dc_time(ctx); break;
        case SERIAL_SYSCALL_STAT:      ser_dc_stat(ctx); break;
        case SERIAL_SYSCALL_UTIME:     ser_dc_utime(ctx); break;
        case SERIAL_SYSCALL_BAD:
            printf("command 15 should not happen... (but it did)\n");
            break;
        case SERIAL_SYSCALL_OPENDIR:   ser_dc_opendir(ctx); break;
        case SERIAL_SYSCALL_CLOSEDIR:  ser_dc_closedir(ctx); break;
        case SERIAL_SYSCALL_READDIR:   ser_dc_readdir(ctx); break;
        case SERIAL_SYSCALL_CDFSREAD:  ser_dc_cdfs_read(ctx); break;
        case SERIAL_SYSCALL_GDBPACKET: ser_dc_gdbpacket(ctx); break;
        case SERIAL_SYSCALL_REWINDDIR: ser_dc_rewinddir(ctx); break;
        case SERIAL_SYSCALL_MKDIR:     ser_dc_mkdir(ctx); break;
        case SERIAL_SYSCALL_PROGEXIT:
            printf("Program returned %d\n", ser_dc_exit(ctx));
            exit(0);
        default:
            printf("Unimplemented command (%d)\n", command);
            printf("Assuming program has exited\n");
            exit(0);
        }
    }
}

/* ===== Network console loop ===== */

static int do_network_console(kostool_context_t *ctx) {
    uint8_t buffer[2048];

    if (ctx->cdfs_enabled && ctx->iso_filename) {
        ctx->cdfs_fd = open(ctx->iso_filename, O_RDONLY | O_BINARY);
        if (ctx->cdfs_fd < 0)
            perror(ctx->iso_filename);
    }

    if (ctx->use_chroot && ctx->chroot_path) {
        ctx->fs_ops->chroot(ctx->chroot_path);
    }

    while (1) {
        fflush(stdout);

        while (net_recv_resp(ctx, buffer, sizeof(buffer), NET_PACKET_TIMEOUT_USEC) == -1)
            ;
        /* Guarantee null termination so strlen() on string commands
         * can never walk past the buffer. */
        buffer[sizeof(buffer) - 1] = '\0';

        if (!memcmp(buffer, NET_CMD_EXIT, 4) || !memcmp(buffer, NET_CMD_PROGEXIT, 4)) {
            net_command_t *exit_cmd = (net_command_t *)buffer;
            int32_t ret_code = (int32_t)ntohl(exit_cmd->address);
            printf("Program returned %d\n", ret_code);
            return 0;
        }
        if (!memcmp(buffer, NET_CMD_FSTAT, 4))      net_dc_fstat(ctx, buffer);
        else if (!memcmp(buffer, NET_CMD_WRITE, 4))  net_dc_write(ctx, buffer);
        else if (!memcmp(buffer, "DD02", 4))         net_dc_write(ctx, buffer); /* legacy */
        else if (!memcmp(buffer, NET_CMD_READ, 4))   net_dc_read(ctx, buffer);
        else if (!memcmp(buffer, NET_CMD_OPEN, 4))   net_dc_open(ctx, buffer);
        else if (!memcmp(buffer, NET_CMD_CLOSE, 4))  net_dc_close(ctx, buffer);
        else if (!memcmp(buffer, NET_CMD_CREAT, 4))  net_dc_creat(ctx, buffer);
        else if (!memcmp(buffer, NET_CMD_LINK, 4))   net_dc_link(ctx, buffer);
        else if (!memcmp(buffer, NET_CMD_UNLINK, 4)) net_dc_unlink(ctx, buffer);
        else if (!memcmp(buffer, NET_CMD_CHDIR, 4))  net_dc_chdir(ctx, buffer);
        else if (!memcmp(buffer, NET_CMD_CHMOD, 4))  net_dc_chmod(ctx, buffer);
        else if (!memcmp(buffer, NET_CMD_LSEEK, 4))  net_dc_lseek(ctx, buffer);
        else if (!memcmp(buffer, NET_CMD_TIME, 4))   net_dc_time(ctx, buffer);
        else if (!memcmp(buffer, NET_CMD_STAT, 4))   net_dc_stat(ctx, buffer);
        else if (!memcmp(buffer, NET_CMD_UTIME, 4))  net_dc_utime(ctx, buffer);
        else if (!memcmp(buffer, NET_CMD_BAD, 4))
            fprintf(stderr, "command 15 should not happen... (but it did)\n");
        else if (!memcmp(buffer, NET_CMD_OPENDIR, 4))   net_dc_opendir(ctx, buffer);
        else if (!memcmp(buffer, NET_CMD_CLOSEDIR, 4))  net_dc_closedir(ctx, buffer);
        else if (!memcmp(buffer, NET_CMD_READDIR, 4))   net_dc_readdir(ctx, buffer);
        else if (!memcmp(buffer, NET_CMD_CDFSREAD, 4))  net_dc_cdfs_read(ctx, buffer);
        else if (!memcmp(buffer, NET_CMD_GDBPACKET, 4)) net_dc_gdbpacket(ctx, buffer);
        else if (!memcmp(buffer, NET_CMD_REWINDDIR, 4)) net_dc_rewinddir(ctx, buffer);
        else if (!memcmp(buffer, NET_CMD_MKDIR, 4))     net_dc_mkdir(ctx, buffer);
    }

    return 0;
}

/* ===== Main console entry point ===== */

int do_console(kostool_context_t *ctx) {
    /* Set up GDB server if requested */
    if (ctx->gdb_enabled) {
        gdb_init(ctx, NET_GDB_PORT);
    }

    if (ctx->dumb_terminal) {
        do_dumbterm(ctx);
        return 0;
    }

    if (strcmp(ctx->transport->name, "serial") == 0)
        return do_serial_console(ctx);
    else
        return do_network_console(ctx);
}
