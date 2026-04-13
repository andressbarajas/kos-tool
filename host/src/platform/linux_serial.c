/* host/src/platform/linux_serial.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>

/* Linux-specific: use termios2 with BOTHER for arbitrary baud rates.
 * asm/termbits.h provides struct termios2 and BOTHER.
 * asm/ioctls.h provides TCGETS2/TCSETS2 (musl doesn't export these
 * through sys/ioctl.h like glibc does). */
#ifdef __linux__
#include <asm/termbits.h>
#include <asm/ioctls.h>
#endif

#include <kostool/platform.h>

typedef struct {
    int fd;
#ifdef __linux__
    struct termios2 old_tio;
#endif
} linux_serial_t;

static void *linux_serial_open(const char *device, uint32_t initial_baud) {
    linux_serial_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->fd = open(device, O_RDWR | O_NOCTTY);
    if (s->fd < 0) {
        perror(device);
        free(s);
        return NULL;
    }

#ifdef __linux__
    ioctl(s->fd, TCGETS2, &s->old_tio);

    struct termios2 newtio;
    memset(&newtio, 0, sizeof(newtio));
    newtio.c_cflag = CRTSCTS | CS8 | CLOCAL | CREAD | BOTHER;
    newtio.c_iflag = IGNPAR | IGNBRK;
    newtio.c_oflag = 0;
    newtio.c_lflag = 0;
    newtio.c_cc[VTIME] = 0;
    newtio.c_cc[VMIN] = 1;
    newtio.c_ospeed = initial_baud;
    newtio.c_cflag |= BOTHER << IBSHIFT;
    newtio.c_ispeed = initial_baud;

    if (ioctl(s->fd, TCSETS2, &newtio) < 0) {
        perror("TCSETS2");
    }
#else
    (void)initial_baud;
#endif

    return s;
}

static void linux_serial_close(void *handle) {
    linux_serial_t *s = handle;
    if (!s) return;
#ifdef __linux__
    ioctl(s->fd, TCSETS2, &s->old_tio);
#endif
    close(s->fd);
    free(s);
}

static int linux_serial_read(void *handle, void *buffer, size_t count) {
    linux_serial_t *s = handle;
    return (int)read(s->fd, buffer, count);
}

static int linux_serial_write(void *handle, const void *buffer, size_t count) {
    linux_serial_t *s = handle;
    return (int)write(s->fd, buffer, count);
}

static int linux_serial_bytes_available(void *handle) {
    linux_serial_t *s = handle;
    int bytes = 0;

    if (ioctl(s->fd, FIONREAD, &bytes) < 0)
        return -1;

    return bytes;
}

static int linux_serial_set_speed(void *handle, uint32_t baud) {
#ifdef __linux__
    linux_serial_t *s = handle;
    struct termios2 tio;
    ioctl(s->fd, TCGETS2, &tio);
    tio.c_cflag &= ~CBAUD;
    tio.c_cflag |= BOTHER;
    tio.c_ospeed = baud;
    tio.c_cflag &= ~(CBAUD << IBSHIFT);
    tio.c_cflag |= BOTHER << IBSHIFT;
    tio.c_ispeed = baud;
    return ioctl(s->fd, TCSETS2, &tio);
#else
    (void)handle; (void)baud;
    return -1;
#endif
}

static void linux_serial_flush(void *handle) {
    linux_serial_t *s = handle;
#ifdef __linux__
    /* Flush both input and output queues.
     * TCFLSH and TCIOFLUSH are from asm/ioctls.h and asm/termbits.h
     * respectively — no conflict since we don't include <termios.h>. */
    ioctl(s->fd, TCFLSH, TCIOFLUSH);
#else
    (void)s;
#endif
}

static void linux_serial_drain(void *handle) {
    linux_serial_t *s = handle;
#ifdef __linux__
    /* Wait until all output has been transmitted.
     * TCSBRK with arg 1 is equivalent to tcdrain() — it waits for
     * output to drain without sending a break. */
    ioctl(s->fd, TCSBRK, 1);
#else
    (void)s;
#endif
}

const platform_serial_ops_t linux_serial_ops = {
    .open = linux_serial_open,
    .close = linux_serial_close,
    .read = linux_serial_read,
    .write = linux_serial_write,
    .bytes_available = linux_serial_bytes_available,
    .set_speed = linux_serial_set_speed,
    .flush = linux_serial_flush,
    .drain = linux_serial_drain,
};

const platform_serial_ops_t *platform_get_serial_ops(void) {
    return &linux_serial_ops;
}
