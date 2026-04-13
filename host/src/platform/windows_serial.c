/* host/src/platform/windows_serial.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <kostool/platform.h>

#ifdef _WIN32

#include <windows.h>

#define DATA_BITS   8
#define PARITY_SET  NOPARITY
#define STOP_BITS   ONESTOPBIT

typedef struct {
    HANDLE hComm;
} win_serial_t;

static void *win_serial_open(const char *device, uint32_t initial_baud) {
    win_serial_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->hComm = CreateFileA(device, GENERIC_READ | GENERIC_WRITE, 0,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (s->hComm == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Error opening %s\n", device);
        free(s);
        return NULL;
    }

    /* Set timeouts */
    COMMTIMEOUTS timeouts;
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = MAXDWORD;
    timeouts.ReadTotalTimeoutConstant = MAXDWORD;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 0;
    SetCommTimeouts(s->hComm, &timeouts);

    /* Configure port */
    DCB dcb;
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(s->hComm, &dcb)) {
        fprintf(stderr, "Error getting serial port state\n");
        CloseHandle(s->hComm);
        free(s);
        return NULL;
    }

    dcb.BaudRate = initial_baud;
    dcb.ByteSize = DATA_BITS;
    dcb.Parity = PARITY_SET;
    dcb.StopBits = STOP_BITS;

    if (!SetCommState(s->hComm, &dcb)) {
        fprintf(stderr, "Error setting serial port state\n");
        CloseHandle(s->hComm);
        free(s);
        return NULL;
    }

    return s;
}

static void win_serial_close(void *handle) {
    win_serial_t *s = handle;
    if (!s) return;
    FlushFileBuffers(s->hComm);
    CloseHandle(s->hComm);
    free(s);
}

static int win_serial_read(void *handle, void *buffer, size_t count) {
    win_serial_t *s = handle;
    DWORD bytesRead = 0;
    if (!ReadFile(s->hComm, buffer, (DWORD)count, &bytesRead, NULL))
        return -1;
    return (int)bytesRead;
}

static int win_serial_write(void *handle, const void *buffer, size_t count) {
    win_serial_t *s = handle;
    DWORD bytesWritten = 0;
    if (!WriteFile(s->hComm, buffer, (DWORD)count, &bytesWritten, NULL))
        return -1;
    return (int)bytesWritten;
}

static int win_serial_bytes_available(void *handle) {
    win_serial_t *s = handle;
    COMSTAT stat;
    DWORD errors = 0;

    if (!ClearCommError(s->hComm, &errors, &stat))
        return -1;

    return (int)stat.cbInQue;
}

static int win_serial_set_speed(void *handle, uint32_t baud) {
    win_serial_t *s = handle;
    DCB dcb;
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(s->hComm, &dcb)) return -1;
    dcb.BaudRate = baud;
    if (!SetCommState(s->hComm, &dcb)) return -1;
    return 0;
}

static void win_serial_flush(void *handle) {
    win_serial_t *s = handle;
    FlushFileBuffers(s->hComm);
}

static void win_serial_drain(void *handle) {
    win_serial_t *s = handle;
    FlushFileBuffers(s->hComm);
}

const platform_serial_ops_t windows_serial_ops = {
    .open = win_serial_open,
    .close = win_serial_close,
    .read = win_serial_read,
    .write = win_serial_write,
    .bytes_available = win_serial_bytes_available,
    .set_speed = win_serial_set_speed,
    .flush = win_serial_flush,
    .drain = win_serial_drain,
};

const platform_serial_ops_t *platform_get_serial_ops(void) {
    return &windows_serial_ops;
}

#endif /* _WIN32 */
