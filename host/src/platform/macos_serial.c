/* host/src/platform/macos_serial.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <IOKit/serial/ioss.h>

#include <kostool/platform.h>

typedef struct {
    int fd;
    struct termios old_tio;
} macos_serial_t;

static void *macos_serial_open(const char *device, uint32_t initial_baud) {
    macos_serial_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->fd = open(device, O_RDWR | O_NOCTTY);
    if (s->fd < 0) {
        perror(device);
        free(s);
        return NULL;
    }

    tcgetattr(s->fd, &s->old_tio);

    struct termios newtio;
    memset(&newtio, 0, sizeof(newtio));
    newtio.c_cflag = CRTSCTS | CS8 | CLOCAL | CREAD;
    newtio.c_iflag = IGNPAR | IGNBRK;
    newtio.c_oflag = 0;
    newtio.c_lflag = 0;
    newtio.c_cc[VTIME] = 0;
    newtio.c_cc[VMIN] = 1;
    cfsetispeed(&newtio, B115200);
    cfsetospeed(&newtio, B115200);

    tcflush(s->fd, TCIOFLUSH);
    tcsetattr(s->fd, TCSANOW, &newtio);

    /* Set actual baud rate via IOSSIOSPEED */
    speed_t speed = initial_baud;
    if (ioctl(s->fd, IOSSIOSPEED, &speed) < 0) {
        perror("IOSSIOSPEED");
    }

    return s;
}

static void macos_serial_close(void *handle) {
    macos_serial_t *s = handle;
    if (!s) return;
    tcflush(s->fd, TCIOFLUSH);
    tcsetattr(s->fd, TCSANOW, &s->old_tio);
    close(s->fd);
    free(s);
}

static int macos_serial_read(void *handle, void *buffer, size_t count) {
    macos_serial_t *s = handle;
    return (int)read(s->fd, buffer, count);
}

static int macos_serial_write(void *handle, const void *buffer, size_t count) {
    macos_serial_t *s = handle;
    return (int)write(s->fd, buffer, count);
}

static int macos_serial_set_speed(void *handle, uint32_t baud) {
    macos_serial_t *s = handle;
    speed_t speed = baud;
    return ioctl(s->fd, IOSSIOSPEED, &speed);
}

static void macos_serial_flush(void *handle) {
    macos_serial_t *s = handle;
    tcflush(s->fd, TCIOFLUSH);
}

static void macos_serial_drain(void *handle) {
    macos_serial_t *s = handle;
    tcdrain(s->fd);
}

const platform_serial_ops_t macos_serial_ops = {
    .open = macos_serial_open,
    .close = macos_serial_close,
    .read = macos_serial_read,
    .write = macos_serial_write,
    .set_speed = macos_serial_set_speed,
    .flush = macos_serial_flush,
    .drain = macos_serial_drain,
};

const platform_serial_ops_t *platform_get_serial_ops(void) {
    return &macos_serial_ops;
}
