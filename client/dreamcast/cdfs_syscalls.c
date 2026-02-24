/* client/dreamcast/cdfs_syscalls.c */
/*
 * CDFS syscall handlers for Dreamcast.
 *
 * Based on dcload-serial: dcload-serial/target-src/dcload/cdfs_syscalls.c
 * (dcload-ip version is nearly identical, differing only in the
 * sector read transport call.)
 *
 * These functions are called by the cdfs_redir.S assembly code
 * to intercept GD-ROM system calls and redirect them to the host.
 */

int gdStatus;

struct TOC {
    unsigned int entry[99];
    unsigned int first, last;
    unsigned int dunno;
};

/* External transport functions provided by serial or network transport */
extern void cdfs_read_sectors(unsigned int start, unsigned int count,
                              unsigned char *dest);

int gdGdcReqCmd(int cmd, int *param) {
    struct TOC *toc;
    int i;

    switch (cmd) {
    case 16: /* read sectors */
        cdfs_read_sectors(param[0], param[1], (unsigned char *)param[2]);
        param[3] = 0;
        gdStatus = 2;
        return 0;
    case 19: /* read TOC */
        toc = (struct TOC *)param[1];
        toc->entry[0] = 0x41000096; /* CTRL=4, ADR=1, LBA=150 */
        for(i = 1; i < 99; i++)
            toc->entry[i] = -1;
        toc->first = 0x41010000;    /* first = track 1 */
        toc->last = 0x41010000;     /* last = track 1 */
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

void gdGdcExecServer(void) { }

int gdGdcGetCmdStat(int f, int *status) {
    if (gdStatus == 0)
        status[0] = 0;
    return gdStatus;
}

void gdGdcGetDrvStat(int *param) {
    param[1] = 32;
}

int gdGdcChangeDataType(int *param) {
    return 0;
}

void gdGdcInitSystem(void) { }
