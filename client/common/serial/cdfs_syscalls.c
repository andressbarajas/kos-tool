/* client/common/serial/cdfs_syscalls.c */
/*
 * CDFS syscall implementations for serial transport.
 * Ported from dcload-serial cdfs_syscalls.c.
 *
 * Intercepts GD-ROM BIOS calls and redirects sector reads over
 * the serial port to the host tool.
 */

#include <kosload/protocol.h>
#include <kosload/serial_io.h>

/* From serial_transport.c */
extern void load_data_block_general(unsigned char *addr,
                                    unsigned int size, unsigned int verbose);
extern unsigned int get_uint(void);
extern void put_uint(unsigned int val);

static int gdStatus = 0;

struct TOC {
    unsigned int entry[99];
    unsigned int first, last;
    unsigned int dunno;
};

int gdGdcReqCmd(int cmd, int *param)
{
    struct TOC *toc;
    int i;

    switch (cmd) {
    case 16: /* read sectors */
        serial_io_putchar(SERIAL_SYSCALL_CDFSREAD);
        put_uint(param[0]); /* starting sector */
        put_uint(param[1]); /* number of sectors */
        load_data_block_general((unsigned char *)param[2], param[1] * 2048, 0);
        param[3] = 0;
        gdStatus = 2;
        return 0;

    case 19: /* read toc */
        toc = (struct TOC *)param[1];
        toc->entry[0] = 0x41000096; /* CTRL = 4, ADR = 1, LBA = 150 */
        for (i = 1; i < 99; i++)
            toc->entry[i] = (unsigned int)-1;
        toc->first = 0x41010000; /* first = track 1 */
        toc->last = 0x41010000;  /* last = track 1 */
        gdStatus = 2;
        return 0;

    case 24: /* init disc */
        gdStatus = 2;
        return 0;

    default:
        gdStatus = 0;
        return -1;
    }
}

void gdGdcExecServer(void)
{
}

int gdGdcGetCmdStat(int f, int *status)
{
    (void)f;
    if (gdStatus == 0)
        status[0] = 0;
    return gdStatus;
}

void gdGdcGetDrvStat(int *param)
{
    param[1] = 32;
}

int gdGdcChangeDataType(int *param)
{
    (void)param;
    return 0;
}

void gdGdcInitSystem(void)
{
}
