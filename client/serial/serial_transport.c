/* client/serial/serial_transport.c */
/*
 * Serial transport implementation for kosload client.
 *
 * Based on dcload-serial: dcload-serial/target-src/dcload/dcload.c
 *
 * Implements the serial command protocol: single-byte commands from
 * the host PC, with LZO-compressed data transfers and XOR checksums.
 * Syscall implementations are in serial_syscalls.c.
 *
 * Platform-independent: uses serial_io.h for byte I/O and target_ops
 * for hardware-specific operations.
 */

#include <string.h>
#include <kosload/target.h>
#include <kosload/transport.h>
#include <kosload/protocol.h>
#include <kosload/serial_io.h>
#include <kosload/types.h>
#include <kosload/info.h>

#include <kosload/screensaver.h>
#include "minilzo.h"
#include "serial_internal.h"

/* Version string */
#ifndef KOSLOAD_VERSION_STRING
#define KOSLOAD_VERSION_STRING "0.1.0"
#endif
#define NAME LOADER_NAME " " KOSLOAD_VERSION_STRING

#define WHITE 0xffff

extern const target_ops_t *common_get_target(void);
extern kosload_info_t kosload_info;

/* ===== Global state ===== */

/* Buffer for LZO compression/decompression.
 * 16384 + 16384/64 + 16 + 3 = 16659 bytes */
static unsigned char compress_buffer[16384 + 256 + 19];

/* Work memory for LZO compression (provided by host via 'G' command,
 * or by loaded programs via assign_wrkmem syscall in serial_syscalls.c) */
unsigned char *wrkmem = 0;

#define BOOTED        1
#define NOT_BOOTED   -1
#define IS(a)        ((a) == BOOTED)
#define IS_NOT(a)    ((a) == NOT_BOOTED)

static int booted = NOT_BOOTED;

/* ===== Serial helpers ===== */

void put_uint(unsigned int val)
{
    serial_io_putchar(val & 0xff);
    serial_io_putchar((val >> 8) & 0xff);
    serial_io_putchar((val >> 16) & 0xff);
    serial_io_putchar((val >> 24) & 0xff);
}

unsigned int get_uint(void)
{
    unsigned int retval;

    retval = 0;
    retval += (serial_io_getchar()) << 24;
    retval >>= 8;
    retval += (serial_io_getchar()) << 24;
    retval >>= 8;
    retval += (serial_io_getchar()) << 24;
    retval >>= 8;
    retval += (serial_io_getchar()) << 24;

    /* Echo back for verification */
    serial_io_putchar(retval & 0xff);
    serial_io_putchar((retval >> 8) & 0xff);
    serial_io_putchar((retval >> 16) & 0xff);
    serial_io_putchar((retval >> 24) & 0xff);

    return retval;
}

/* ===== Data transfer helpers ===== */

static unsigned int send_data_block_uncompressed(unsigned char *addr, unsigned int size)
{
    unsigned int i;
    unsigned char *location = addr;
    unsigned char sum = 0;
    unsigned char data;

    serial_io_putchar('U');
    put_uint(size);

    for (i = 0; i < size; i++) {
        data = *(location++);
        serial_io_putchar(data);
        sum ^= data;
    }

    serial_io_putchar(sum);
    data = serial_io_getchar();

    return (data == 'G') ? 1 : 0;
}

unsigned int send_data_block_compressed(unsigned char *addr, unsigned int size)
{
    unsigned int i;
    unsigned char *location;
    unsigned char sum = 0;
    unsigned char data;
    lzo_uint csize;
    unsigned int sendsize;

    if (!wrkmem)
        return send_data_block_uncompressed(addr, size);

    if (size < 19)
        return send_data_block_uncompressed(addr, size);

    while (size) {
        if (size > 8192)
            sendsize = 8192;
        else
            sendsize = size;

        lzo1x_1_compress(addr, sendsize, compress_buffer, &csize, wrkmem);

        if (csize < sendsize) {
            /* Send compressed */
            serial_io_putchar('C');
            put_uint(csize);
            data = 'B';
            while (data != 'G') {
                location = compress_buffer;
                sum = 0;
                for (i = 0; i < csize; i++) {
                    data = *(location++);
                    serial_io_putchar(data);
                    sum ^= data;
                }
                serial_io_putchar(sum);
                data = serial_io_getchar();
            }
        } else {
            /* Compression didn't help, send uncompressed */
            while (!send_data_block_uncompressed(addr, sendsize))
                ;
        }
        size -= sendsize;
        addr += sendsize;
    }

    return 1;
}

void load_data_block_general(unsigned char *addr, unsigned int total, unsigned int verbose)
{
    unsigned char type, sum;
    lzo_uint size, newsize;
    unsigned char *tmp = compress_buffer;
    unsigned int i;
    unsigned char *data = addr;
    unsigned int realtotal = total;

    const target_ops_t *t = common_get_target();

    while (total) {
        if (verbose) {
            unsigned char current_str[9], total_str[9];
            extern void uint_to_string(unsigned int foo, unsigned char *bar);
            extern void clear_lines(int y, int height, unsigned int color);

            uint_to_string(realtotal, total_str);
            uint_to_string(realtotal - total, current_str);

            clear_lines(102, 24, 0);
            t->draw_string(30, 102, "(", 0xffff);
            t->draw_string(42, 102, (const char *)current_str, 0xffff);
            t->draw_string(138, 102, "/", 0xffff);
            t->draw_string(150, 102, (const char *)total_str, 0xffff);
            t->draw_string(246, 102, ")", 0xffff);
        }

        type = serial_io_getchar();
        size = get_uint();

        switch (type) {
        case 'U': /* uncompressed */
            for (i = 0; i < size; i++)
                *(data++) = serial_io_getchar();
            sum = serial_io_getchar();
            (void)sum; /* checksum not verified — retry protocol is broken at
                        * the framing level (host resends data+checksum only,
                        * client expects type+size+data+checksum) */
            serial_io_putchar('G');
            total = (size >= total) ? 0 : total - size;
            break;

        case 'C': /* compressed */
            for (i = 0; i < size; i++)
                tmp[i] = serial_io_getchar();
            sum = serial_io_getchar();
            (void)sum; /* same as above — checksum not verified */
            if (lzo1x_decompress(tmp, size, data, &newsize, 0) == LZO_E_OK) {
                serial_io_putchar('G');
                total = (newsize >= total) ? 0 : total - newsize;
                data += newsize;
            } else {
                serial_io_putchar('B');
            }
            break;

        default:
            break;
        }
    }
}

/* ===== Transport main loop ===== */

extern void clear_lines(int y, int height, unsigned int color);

static void serial_restore_screen(void)
{
    const target_ops_t *t = common_get_target();
    t->setup_video(0, 0);
    t->clear_screen(0);
    t->draw_string(30, 54, NAME, 0xffff);
    clear_lines(78, 24, 0);
    t->draw_string(30, 78, "idle...", 0xffff);
}

static int serial_transport_init(void)
{
    if (serial_io_init(SERIAL_DEFAULT_SPEED) != 0)
        return -1;
    lzo_init();
    wrkmem = 0;
    booted = NOT_BOOTED;
    kosload_info.capabilities = KOSLOAD_CAP_SERIAL;
    if (target_get_ops()->cdfs_redir_enable)
        kosload_info.capabilities |= KOSLOAD_CAP_CDFS_REDIR;
    kosload_info.transport = KOSLOAD_TRANSPORT_SERIAL;
    kosload_info.baud_rate = SERIAL_DEFAULT_SPEED;
    screensaver_init(serial_restore_screen, WHITE);
    return 0;
}

static void serial_transport_loop(bool is_main_loop)
{
    unsigned char cmd;
    unsigned int addr;
    unsigned int size;
    unsigned int console;

    const target_ops_t *t = common_get_target();

    (void)is_main_loop;

    while (1) {
        serial_io_set_border(0x00ffffff);

        if (IS_NOT(booted)) {
            t->setup_video(0, 0);
            t->clear_screen(0);
            t->draw_string(30, 54, NAME, 0xffff);
            booted = BOOTED;
        }

        clear_lines(78, 24, 0);
        t->draw_string(30, 78, "idle...", 0xffff);

        screensaver_reset();
        while (!serial_io_data_ready())
            screensaver_poll();
        screensaver_wake();

        cmd = serial_io_getchar();
        serial_io_putchar(cmd);

        switch (cmd) {
        case SERIAL_CMD_EXECUTE: /* 'A' - execute */
            if (IS_NOT(booted)) {
                t->setup_video(0, 0);
                t->draw_string(30, 54, NAME, 0xffff);
                booted = BOOTED;
            }
            clear_lines(78, 24, 0);
            t->draw_string(30, 78, "executing...", 0xffff);

            addr = get_uint();
            console = get_uint();

            /* Bit 31 of console field signals "args follow".
             * Legacy dc-tool-ser never sets this bit, so kosload-serial
             * stays backwards-compatible with the old host tool. */
            kosload_info.argc = 0;
            kosload_info.command_line[0] = '\0';

            if (console & (1u << 31)) {
                console &= ~(1u << 31); /* clear flag before use */
                unsigned int prog_argc = get_uint();
                if (prog_argc > 0) {
                    unsigned int cmdline_len = get_uint();
                    if (cmdline_len > KOSLOAD_MAX_CMDLINE)
                        cmdline_len = KOSLOAD_MAX_CMDLINE;
                    for (unsigned int ci = 0; ci < cmdline_len; ci++)
                        kosload_info.command_line[ci] = serial_io_getchar();
                    kosload_info.command_line[cmdline_len - 1] = '\0';
                    kosload_info.argc = prog_argc;
                }
            }

            t->set_console_enabled(console);
            serial_io_flush();
            t->execute(addr);
            booted = NOT_BOOTED; /* reinit video on next loop */
            break;

        case SERIAL_CMD_LOAD_BEGIN: /* 'B' - load binary */
            if (IS_NOT(booted)) {
                t->setup_video(0, 0);
                t->draw_string(30, 54, NAME, 0xffff);
                booted = BOOTED;
            }
            clear_lines(78, 24, 0);
            t->draw_string(30, 78, "receiving data...", 0xffff);

            addr = get_uint();
            size = get_uint();
            load_data_block_general((unsigned char *)addr, size, 1);
            break;

        case 'D': /* send uncompressed binary (verbose) */
            if (IS_NOT(booted)) {
                t->setup_video(0, 0);
                t->draw_string(30, 54, NAME, 0xffff);
                booted = BOOTED;
            }
            clear_lines(78, 24, 0);
            t->draw_string(30, 78, "sending uncompressed data...", 0xffff);
            /* fall through */
        case 'E': /* send uncompressed binary (quiet) */
            addr = get_uint();
            size = get_uint();
            send_data_block_uncompressed((unsigned char *)addr, size);
            break;

        case SERIAL_CMD_DOWNLOAD: /* 'F' - send compressed binary (verbose) */
            if (IS_NOT(booted)) {
                t->setup_video(0, 0);
                t->draw_string(30, 54, NAME, 0xffff);
                booted = BOOTED;
            }
            clear_lines(78, 24, 0);
            t->draw_string(30, 78, "sending compressed data...", 0xffff);
            /* fall through */
        case SERIAL_CMD_DOWNLOAD_Q: /* 'G' - send compressed binary (quiet) */
            addr = get_uint();
            size = get_uint();
            wrkmem = (unsigned char *)get_uint();
            send_data_block_compressed((unsigned char *)addr, size);
            wrkmem = 0;
            break;

        case SERIAL_CMD_CDFS_REDIR: /* 'H' - enable CDFS redirect */
            t->cdfs_redir_enable();
            break;

        case SERIAL_CMD_SPEED: /* 'S' - change serial speed */
            addr = get_uint();
            serial_io_flush();
            serial_io_init(addr);
            kosload_info.baud_rate = addr;
            addr = get_uint();
            put_uint(addr);
            break;

        case SERIAL_CMD_SETRTC: /* 'W' - set RTC */
        {
            unsigned int timestamp = get_uint();
            t->set_rtc(timestamp);
            break;
        }

        case 'V': /* version */
            serial_io_puts((const unsigned char *)NAME);
            serial_io_puts((const unsigned char *)"\n");
            break;

        default:
            serial_io_init(SERIAL_DEFAULT_SPEED);
            break;
        }
    }
}

static int serial_transport_syscall_send(const char cmd_id[4],
                                         const uint8_t *payload,
                                         size_t payload_len)
{
    /* Serial syscalls use their own protocol directly, not this callback */
    (void)cmd_id; (void)payload; (void)payload_len;
    return -1;
}

static void serial_transport_exit_notify(void)
{
    /* Handled by the progexit() syscall directly */
}

static void serial_transport_stop(void)
{
    /* No-op for serial */
}

static void serial_transport_start(void)
{
    /* No-op for serial */
}

const client_transport_ops_t client_serial_transport_ops = {
    .name = "serial",
    .init_error_msg = "NO SERIAL ADAPTER DETECTED!",
    .init = serial_transport_init,
    .loop = serial_transport_loop,
    .syscall_send = serial_transport_syscall_send,
    .exit_notify = serial_transport_exit_notify,
    .stop = serial_transport_stop,
    .start = serial_transport_start,
};
