/* client/common/network/cdfs_syscalls.c */
/*
 * CDFS syscall implementations for network transport.
 * Based on dcload-ip: dcload-ip/target-src/dcload/cdfs_syscalls.c
 *
 * Intercepts GD-ROM BIOS calls and redirects sector reads over
 * the network to the host tool via CMD_CDFSREAD.
 */

#include <string.h>
#include <kosload/protocol.h>
#include <kosload/net_adapter.h>
#include <kosload/net_stack.h>
#include "packet.h"
#include "commands.h"

/* From network_syscalls.c */
extern void build_send_packet(int command_len);

static int gdStatus = 0;

struct TOC {
    unsigned int entry[99];
    unsigned int first, last;
    unsigned int dunno;
};

int gdGdcReqCmd(int cmd, int *param) {
    net_command_3int_t *command = (net_command_3int_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);
    struct TOC *toc;
    int i;

    switch(cmd) {
    case 16: /* read sectors */
        memcpy(command->id, NET_SYSCALL_CDFSREAD, 4);
        command->value0 = htonl(param[0]);
        command->value1 = htonl(param[2]);
        command->value2 = htonl(param[1] * 2048);
        build_send_packet(sizeof(net_command_3int_t));
        bb->loop(0);

        param[3] = 0;
        gdStatus = 2;
        return 0;

    case 19: /* read toc */
        toc = (struct TOC *)param[1];
        toc->entry[0] = 0x41000096; /* CTRL = 4, ADR = 1, LBA = 150 */
        for(i = 1; i < 99; i++)
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

void gdGdcExecServer(void) {
}

int gdGdcGetCmdStat(int f, int *status) {
    (void)f;
    if(gdStatus == 0)
        status[0] = 0;
    return gdStatus;
}

void gdGdcGetDrvStat(int *param) {
    param[1] = 32;
}

int gdGdcChangeDataType(int *param) {
    (void)param;
    return 0;
}

void gdGdcInitSystem(void) {
}
