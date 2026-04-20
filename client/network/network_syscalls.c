/* client/network/network_syscalls.c */
/*
 * Network transport syscall implementations for kosload client.
 * Based on dcload-ip: dcload-ip/target-src/dcload/syscalls.c
 *
 * These functions implement POSIX-like file I/O by sending UDP command
 * packets to the host tool and waiting for CMD_RETVAL responses.
 * Each syscall builds a packet in pkt_buf, calls build_send_packet()
 * to transmit it, then enters bb->loop(0) to wait for the response.
 *
 * The crt0.S syscall table maps slots 0-21 to these functions.
 */

#include <string.h>
#include <kosload/protocol.h>
#include <kosload/types.h>
#include <kosload/net_adapter.h>
#include <kosload/net_stack.h>
#include "commands.h"
#include "packet.h"

/* ===== Syscall shared state ===== */

unsigned short dcload_syscall_port = 31313;
unsigned int syscall_retval = 0;
unsigned char *syscall_data = 0;

/* Private dirent buffer for readdir */
typedef struct {
    long d_ino;
    long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[256];
} dc_dirent_t;

static dc_dirent_t our_dir;

/* ===== strlen (no libc available) ===== */

static unsigned int my_strlen(const char *s)
{
    unsigned int c = 0;
    while (s[c] != 0)
        c++;
    return c;
}

/* ===== Packet construction helper ===== */

void build_send_packet(int command_len)
{
    ether_header_t *ether = (ether_header_t *)pkt_buf;
    ip_header_t *ip = (ip_header_t *)(pkt_buf + ETHER_H_LEN);
    udp_header_t *udp = (udp_header_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN);

    make_ether(tool_mac, bb->mac, ether);
    make_ip(tool_ip, our_ip, UDP_H_LEN + command_len, IP_UDP_PROTOCOL, ip, 0);
    make_udp(tool_port, dcload_syscall_port, command_len, ip, udp);
    bb->start();
    bb->tx(pkt_buf, ETHER_H_LEN + IP_H_LEN + UDP_H_LEN + command_len);
}

/* Syscall 14: assign_wrkmem — not needed for network transport
 * (compression is handled differently), but must exist for crt0.S linkage. */
void assign_wrkmem(unsigned char *user_buffer)
{
    (void)user_buffer;
}

/* ===== Syscall implementations ===== */

void progexit(int ret_code)
{
    bb->stop();

    net_command_t *command = (net_command_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);

    memcpy(command->id, NET_SYSCALL_PROGEXIT, 4);
    command->address = htonl((unsigned int)ret_code);
    command->size = 0;
    build_send_packet(COMMAND_LEN);
}

int read(int fd, void *buf, unsigned int count)
{
    net_command_3int_t *command = (net_command_3int_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);

    memcpy(command->id, NET_SYSCALL_READ, 4);
    command->value0 = htonl(fd);
    command->value1 = htonl((unsigned int)buf);
    command->value2 = htonl(count);
    build_send_packet(sizeof(net_command_3int_t));
    bb->loop(0);

    return syscall_retval;
}

int write(int fd, const void *buf, unsigned int count)
{
    net_command_3int_t *command = (net_command_3int_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);

    if (DCTOOL_MAJOR < 2)
        memcpy(command->id, NET_SYSCALL_WRITE_OLD, 4);
    else
        memcpy(command->id, NET_SYSCALL_WRITE, 4);

    command->value0 = htonl(fd);
    command->value1 = htonl((unsigned int)buf);
    command->value2 = htonl(count);
    build_send_packet(sizeof(net_command_3int_t));
    bb->loop(0);

    return syscall_retval;
}

int open(const char *pathname, int flags, int mode)
{
    net_command_2int_string_t *command = (net_command_2int_string_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);
    int namelen = my_strlen(pathname);

    memcpy(command->id, NET_SYSCALL_OPEN, 4);
    command->value0 = htonl(flags);
    command->value1 = htonl(mode);
    memcpy(command->string, pathname, namelen + 1);

    build_send_packet(sizeof(net_command_2int_string_t) + namelen + 1);
    bb->loop(0);

    return syscall_retval;
}

int close(int fd)
{
    net_command_int_t *command = (net_command_int_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);

    memcpy(command->id, NET_SYSCALL_CLOSE, 4);
    command->value0 = htonl(fd);

    build_send_packet(sizeof(net_command_int_t));
    bb->loop(0);

    return syscall_retval;
}

int creat(const char *pathname, int mode)
{
    net_command_int_string_t *command = (net_command_int_string_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);
    int namelen = my_strlen(pathname);

    memcpy(command->id, NET_SYSCALL_CREAT, 4);
    command->value0 = htonl(mode);
    memcpy(command->string, pathname, namelen + 1);

    build_send_packet(sizeof(net_command_int_string_t) + namelen + 1);
    bb->loop(0);

    return syscall_retval;
}

int link(const char *oldpath, const char *newpath)
{
    net_command_string_t *command = (net_command_string_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);
    int namelen1 = my_strlen(oldpath);
    int namelen2 = my_strlen(newpath);

    memcpy(command->id, NET_SYSCALL_LINK, 4);
    memcpy(command->string, oldpath, namelen1 + 1);
    memcpy(command->string + namelen1 + 1, newpath, namelen2 + 1);

    build_send_packet(sizeof(net_command_string_t) + namelen1 + namelen2 + 2);
    bb->loop(0);

    return syscall_retval;
}

int unlink(const char *pathname)
{
    net_command_string_t *command = (net_command_string_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);
    int namelen = my_strlen(pathname);

    memcpy(command->id, NET_SYSCALL_UNLINK, 4);
    memcpy(command->string, pathname, namelen + 1);

    build_send_packet(sizeof(net_command_string_t) + namelen + 1);
    bb->loop(0);

    return syscall_retval;
}

int chdir(const char *path)
{
    net_command_string_t *command = (net_command_string_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);
    int namelen = my_strlen(path);

    memcpy(command->id, NET_SYSCALL_CHDIR, 4);
    memcpy(command->string, path, namelen + 1);

    build_send_packet(sizeof(net_command_string_t) + namelen + 1);
    bb->loop(0);

    return syscall_retval;
}

int chmod(const char *path, int mode)
{
    net_command_int_string_t *command = (net_command_int_string_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);
    int namelen = my_strlen(path);

    memcpy(command->id, NET_SYSCALL_CHMOD, 4);
    command->value0 = htonl(mode);
    memcpy(command->string, path, namelen + 1);

    build_send_packet(sizeof(net_command_int_string_t) + namelen + 1);
    bb->loop(0);

    return syscall_retval;
}

int mkdir(const char *path, int mode)
{
    net_command_int_string_t *command = (net_command_int_string_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);
    int namelen = my_strlen(path);

    memcpy(command->id, NET_SYSCALL_MKDIR, 4);
    command->value0 = htonl(mode);
    memcpy(command->string, path, namelen + 1);

    build_send_packet(sizeof(net_command_int_string_t) + namelen + 1);
    bb->loop(0);

    return syscall_retval;
}

int lseek(int fd, int offset, int whence)
{
    net_command_3int_t *command = (net_command_3int_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);

    memcpy(command->id, NET_SYSCALL_LSEEK, 4);
    command->value0 = htonl(fd);
    command->value1 = htonl(offset);
    command->value2 = htonl(whence);

    build_send_packet(sizeof(net_command_3int_t));
    bb->loop(0);

    return syscall_retval;
}

int fstat(int fd, void *buf)
{
    net_command_3int_t *command = (net_command_3int_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);

    memcpy(command->id, NET_SYSCALL_FSTAT, 4);
    command->value0 = htonl(fd);
    command->value1 = htonl((unsigned int)buf);
    command->value2 = htonl(sizeof(dcload_stat_t));

    build_send_packet(sizeof(net_command_3int_t));
    bb->loop(0);

    return syscall_retval;
}

int time(unsigned int *t)
{
    net_command_int_t *command = (net_command_int_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);

    memcpy(command->id, NET_SYSCALL_TIME, 4);
    command->value0 = htonl((unsigned int)t);

    build_send_packet(sizeof(net_command_int_t));
    bb->loop(0);

    if (t != 0)
        *t = syscall_retval;

    return syscall_retval;
}

int stat(const char *pathname, void *buf)
{
    net_command_2int_string_t *command = (net_command_2int_string_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);
    int namelen = my_strlen(pathname);

    memcpy(command->id, NET_SYSCALL_STAT, 4);
    memcpy(command->string, pathname, namelen + 1);
    command->value0 = htonl((unsigned int)buf);
    command->value1 = htonl(sizeof(dcload_stat_t));

    build_send_packet(sizeof(net_command_2int_string_t) + namelen + 1);
    bb->loop(0);

    return syscall_retval;
}

int utime(const char *filename, void *buf)
{
    net_command_3int_string_t *command = (net_command_3int_string_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);
    int namelen = my_strlen(filename);
    unsigned int *times = (unsigned int *)buf;

    memcpy(command->id, NET_SYSCALL_UTIME, 4);
    memcpy(command->string, filename, namelen + 1);
    command->value0 = htonl((unsigned int)buf);

    if (buf) {
        command->value1 = htonl(times[0]); /* actime */
        command->value2 = htonl(times[1]); /* modtime */
    }

    build_send_packet(sizeof(net_command_3int_string_t) + namelen + 1);
    bb->loop(0);

    return syscall_retval;
}

unsigned int opendir(const char *name)
{
    net_command_string_t *command = (net_command_string_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);
    int namelen = my_strlen(name);

    memcpy(command->id, NET_SYSCALL_OPENDIR, 4);
    memcpy(command->string, name, namelen + 1);

    build_send_packet(sizeof(net_command_string_t) + namelen + 1);
    bb->loop(0);

    return syscall_retval;
}

int closedir(unsigned int dir)
{
    net_command_int_t *command = (net_command_int_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);

    memcpy(command->id, NET_SYSCALL_CLOSEDIR, 4);
    command->value0 = htonl(dir);

    build_send_packet(sizeof(net_command_int_t));
    bb->loop(0);

    return syscall_retval;
}

void *readdir(unsigned int dir)
{
    net_command_3int_t *command = (net_command_3int_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);

    memcpy(command->id, NET_SYSCALL_READDIR, 4);
    command->value0 = htonl(dir);
    command->value1 = htonl((unsigned int)&our_dir);
    command->value2 = htonl(sizeof(dc_dirent_t));

    build_send_packet(sizeof(net_command_3int_t));
    bb->loop(0);

    if (syscall_retval)
        return &our_dir;
    else
        return 0;
}

unsigned int gdbpacket(const char *in_buf, unsigned int size_pack, char *out_buf)
{
    unsigned int in_size = size_pack >> 16;
    unsigned int out_size = size_pack & 0xffff;
    net_command_2int_string_t *command = (net_command_2int_string_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);

    memcpy(command->id, NET_SYSCALL_GDBPACKET, 4);
    command->value0 = htonl(in_size);
    command->value1 = htonl(out_size);
    memcpy(command->string, in_buf, in_size);
    build_send_packet(sizeof(net_command_2int_string_t) + in_size);
    bb->loop(0);

    if (syscall_retval <= out_size)
        memcpy(out_buf, syscall_data, syscall_retval);

    return syscall_retval;
}

int rewinddir(unsigned int dir)
{
    net_command_int_t *command = (net_command_int_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);

    memcpy(command->id, NET_SYSCALL_REWINDDIR, 4);
    command->value0 = htonl(dir);

    build_send_packet(sizeof(net_command_int_t));
    bb->loop(0);

    return syscall_retval;
}

int gethostinfo(unsigned int *ip, unsigned int *port)
{
    *ip = tool_ip;
    *port = tool_port;
    return our_ip;
}
