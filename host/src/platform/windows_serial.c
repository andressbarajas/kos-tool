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

static int win_serial_set_timeouts(HANDLE hComm)
{
    COMMTIMEOUTS timeouts;

    memset(&timeouts, 0, sizeof(timeouts));

    /* Match the transport's blocking-read expectations:
     * ReadFile waits until the requested byte count is available. */
    timeouts.ReadIntervalTimeout = 0;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 0;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 0;

    return SetCommTimeouts(hComm, &timeouts) ? 0 : -1;
}

static int win_serial_configure_port(HANDLE hComm, uint32_t baud)
{
    DCB dcb;

    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);

    if (!GetCommState(hComm, &dcb))
        return -1;

    dcb.BaudRate = baud;
    dcb.ByteSize = DATA_BITS;
    dcb.Parity = PARITY_SET;
    dcb.StopBits = STOP_BITS;

    /* Use raw 8N1 with hardware flow control, matching the POSIX backends. */
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = TRUE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fTXContinueOnXoff = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fErrorChar = FALSE;
    dcb.fNull = FALSE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fAbortOnError = FALSE;

    if (!SetCommState(hComm, &dcb))
        return -1;

    return PurgeComm(hComm,
                     PURGE_RXABORT | PURGE_RXCLEAR |
                     PURGE_TXABORT | PURGE_TXCLEAR) ? 0 : -1;
}

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

    if (!SetupComm(s->hComm, 4096, 4096)) {
        fprintf(stderr, "Error setting serial queue sizes\n");
        CloseHandle(s->hComm);
        free(s);
        return NULL;
    }

    if (win_serial_set_timeouts(s->hComm) != 0) {
        fprintf(stderr, "Error setting serial port timeouts\n");
        CloseHandle(s->hComm);
        free(s);
        return NULL;
    }

    if (win_serial_configure_port(s->hComm, initial_baud) != 0) {
        fprintf(stderr, "Error configuring serial port state\n");
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

    return win_serial_configure_port(s->hComm, baud);
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
