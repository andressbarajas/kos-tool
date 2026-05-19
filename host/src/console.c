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
#endif

#include <kosload/file_compat.h>
#include <kosload/protocol.h>
#include <kosload/strutil.h>
#include <kosload/types.h>
#include <kostool/transport.h>
#include <kostool/platform.h>
#include <kostool/gdb.h>
#include <kostool/cdfs.h>
#include "minilzo.h"

#define MAX_PATH_LEN 4096
#define MAX_SYSCALL_SIZE (32 * 1024 * 1024)  /* 32MB sanity cap for remote malloc */
#define SERIAL_EXIT_CODE_PROBE_USEC 100000

static int host_mkdir(const char *path, int mode) {
#ifdef _WIN32
    (void)mode;
    return _mkdir(path);
#else
    return mkdir(path, mode);
#endif
}

/* Check if a file starts with ELF magic (\x7fELF).
 * addr2line only works on ELF files, so skip it for .bin/.dol/etc. */
static int is_elf_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    unsigned char magic[4];
    int ok = fread(magic, 1, 4, f) == 4 &&
             magic[0] == 0x7f && magic[1] == 'E' &&
             magic[2] == 'L' && magic[3] == 'F';
    fclose(f);
    return ok;
}

/* ===== addr2line address decoding ===== */

#define ADDR2LINE_CACHE_SIZE 64

static struct {
    uint32_t addr;
    char decoded[256];
} addr_cache[ADDR2LINE_CACHE_SIZE];
static int addr_cache_count = 0;

/* Persistent addr2line process — avoids fork/exec per address.
 * Started lazily on first use, stays alive for the session. */
static FILE *a2l_in = NULL;     /* parent writes addresses here */
static FILE *a2l_out = NULL;    /* parent reads results here */

#ifndef _WIN32
#include <signal.h>

static void start_addr2line_process(const char *cmd, const char *elf) {
    int to_child[2], from_child[2];

    if (pipe(to_child) < 0)
        return;
    if (pipe(from_child) < 0) {
        close(to_child[0]); close(to_child[1]);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        return;
    }

    if (pid == 0) {
        /* Child: wire pipes and exec addr2line in interactive mode */
        close(to_child[1]);
        close(from_child[0]);
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        close(to_child[0]);
        close(from_child[1]);
        execlp(cmd, cmd, "-Cfpe", elf, NULL);
        _exit(1);
    }

    /* Parent */
    close(to_child[0]);
    close(from_child[1]);
    a2l_in = fdopen(to_child[1], "w");
    a2l_out = fdopen(from_child[0], "r");

    if (!a2l_in || !a2l_out) {
        if (a2l_in) fclose(a2l_in);
        if (a2l_out) fclose(a2l_out);
        a2l_in = a2l_out = NULL;
        return;
    }

    /* Unbuffered write so addresses are sent immediately */
    setbuf(a2l_in, NULL);

    /* Auto-reap child on exit */
    signal(SIGCHLD, SIG_IGN);
}
#endif /* !_WIN32 */

static void decode_address(const char *addr2line_cmd, const char *elf_path,
                           uint32_t addr, char *out, size_t out_size) {
    int i;

    /* Check cache */
    for (i = 0; i < addr_cache_count; i++) {
        if (addr_cache[i].addr == addr) {
            compat_str_copy(out, out_size, addr_cache[i].decoded);
            return;
        }
    }

    out[0] = '\0';

    if (a2l_in && a2l_out) {
        /* Persistent process: write address, read one line */
        fprintf(a2l_in, "0x%08x\n", addr);
        if (fgets(out, (int)out_size, a2l_out) == NULL)
            out[0] = '\0';
    } else {
        /* Fallback: spawn per-address */
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "%s -Cifpe %s 0x%08x",
                 addr2line_cmd, elf_path, addr);
        FILE *fp = popen(cmd, "r");
        if (fp) {
            if (fgets(out, (int)out_size, fp) == NULL)
                out[0] = '\0';
            pclose(fp);
        }
    }

    /* Strip trailing newline */
    size_t len = strlen(out);
    if (len > 0 && out[len - 1] == '\n') out[len - 1] = '\0';

    /* Don't cache useless results like "?? ??:0" */
    if (out[0] == '?' || out[0] == '\0')
        return;

    /* Cache result */
    if (addr_cache_count < ADDR2LINE_CACHE_SIZE) {
        addr_cache[addr_cache_count].addr = addr;
        compat_str_copy(addr_cache[addr_cache_count].decoded,
                        sizeof(addr_cache[addr_cache_count].decoded), out);
        addr_cache_count++;
    }
}

/* ===== Stack trace annotation ===== */

static int in_stack_trace = 0;
static char stk_line_buf[256];
static int stk_line_buf_len = 0;
static int addr2line_available = -1; /* -1 = unchecked, 0 = no, 1 = yes */

/* Parse 8-digit hex address from "   XXXXXXXX" format.
 * Returns 1 on success, 0 on failure. */
static int parse_trace_addr(const char *line, size_t len, uint32_t *addr) {
    int i;
    uint32_t val = 0;

    if (len < 12 || line[0] != ' ' || line[1] != ' ' || line[2] != ' ')
        return 0;

    for (i = 3; i < 11 && i < (int)len; i++) {
        char c = line[i];
        if (c >= '0' && c <= '9')      val = (val << 4) | (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') val = (val << 4) | (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') val = (val << 4) | (uint32_t)(c - 'A' + 10);
        else return 0;
    }

    if (i != 11) return 0;
    *addr = val;
    return 1;
}

#define STK_BANNER_PREFIX "-------- "
#define STK_BANNER_PREFIX_LEN 9

/* Process a single complete line, annotating stack trace addresses
 * with addr2line results when available. */
static void console_write_line(const char *addr2line_cmd, const char *elf_path,
                               int fd, const char *line, size_t len) {
    /* Detect stack trace banners via fixed prefix */
    if (len > STK_BANNER_PREFIX_LEN &&
        memcmp(line, STK_BANNER_PREFIX, STK_BANNER_PREFIX_LEN) == 0) {
        if (len > 20 && line[STK_BANNER_PREFIX_LEN] == 'S') {
            in_stack_trace = 1;
        } else if (line[STK_BANNER_PREFIX_LEN] == 'E') {
            in_stack_trace = 0;
        }
        write(fd, line, len);
        return;
    }

    /* Inside a trace: try to parse address lines */
    if (in_stack_trace) {
        uint32_t addr;

        if (parse_trace_addr(line, len, &addr)) {
            char decoded[256];

            decode_address(addr2line_cmd, elf_path, addr, decoded,
                           sizeof(decoded));

            if (decoded[0] && decoded[0] != '?') {
                /* Build annotated line in one buffer, one write */
                char outbuf[512];
                size_t trim = len;
                size_t out_len;

                while (trim > 0 && (line[trim - 1] == '\n' ||
                       line[trim - 1] == '\r'))
                    trim--;

                out_len = compat_str_append_bytes(outbuf, sizeof(outbuf), 0,
                                                  line, trim);
                out_len = compat_str_append_bytes(outbuf, sizeof(outbuf), out_len,
                                                  "   ", 3);
                out_len = compat_str_append(outbuf, sizeof(outbuf), out_len,
                                            decoded);
                out_len = compat_str_append_bytes(outbuf, sizeof(outbuf), out_len,
                                                  "\n", 1);
                write(fd, outbuf, out_len);
                return;
            }
        }
    }

    /* Default: pass through unchanged */
    write(fd, line, len);
}

/* Write console output, annotating stack trace addresses with addr2line.
 * Handles line buffering for data that doesn't end on a line boundary. */
static int console_write(kostool_context_t *ctx, int fd,
                         const uint8_t *data, uint32_t count) {
    const char *addr2line_cmd = ctx->target_big_endian ?
                                ctx->ppc_addr2line : ctx->sh4_addr2line;

    /* Check addr2line availability once, then cache the result */
    if (addr2line_available < 0) {
        addr2line_available = ctx->loaded_binary_path &&
                              addr2line_cmd[0] &&
                              access(addr2line_cmd, X_OK) == 0 &&
                              is_elf_file(ctx->loaded_binary_path);
#ifndef _WIN32
        if (addr2line_available)
            start_addr2line_process(addr2line_cmd, ctx->loaded_binary_path);
#endif
    }

    if (!addr2line_available)
        return write(fd, data, count);

    /* Fast path: not in a trace, no buffered partial line, and the
     * data doesn't start with the banner dash prefix.  KOS sends each
     * dbgio_printf() as a complete write, so the banner always appears
     * at the start of a chunk.  Checking data[0] is O(1) vs scanning
     * the whole buffer for any hyphen. */
    if (!in_stack_trace && stk_line_buf_len == 0 &&
        (count == 0 || data[0] != '-')) {
        return write(fd, data, count);
    }

    const char *p = (const char *)data;
    const char *end = p + count;

    while (p < end) {
        const char *nl = memchr(p, '\n', end - p);

        if (!nl) {
            /* No newline — buffer the remainder for next call */
            size_t rem = end - p;

            if (stk_line_buf_len + (int)rem < (int)sizeof(stk_line_buf)) {
                memcpy(stk_line_buf + stk_line_buf_len, p, rem);
                stk_line_buf_len += (int)rem;
            } else {
                /* Buffer overflow — flush raw */
                if (stk_line_buf_len > 0) {
                    write(fd, stk_line_buf, stk_line_buf_len);
                    stk_line_buf_len = 0;
                }
                write(fd, p, rem);
            }
            break;
        }

        /* Complete line (including the \n) */
        size_t line_len = nl - p + 1;

        if (stk_line_buf_len > 0) {
            /* Prepend buffered partial line */
            size_t total = stk_line_buf_len + line_len;

            if (total < sizeof(stk_line_buf)) {
                memcpy(stk_line_buf + stk_line_buf_len, p, line_len);
                console_write_line(addr2line_cmd, ctx->loaded_binary_path,
                                   fd, stk_line_buf, total);
            } else {
                write(fd, stk_line_buf, stk_line_buf_len);
                write(fd, p, line_len);
            }
            stk_line_buf_len = 0;
        } else {
            console_write_line(addr2line_cmd, ctx->loaded_binary_path,
                               fd, p, line_len);
        }

        p = nl + 1;
    }

    return (int)count;
}

/* ===== Exception handling ===== */

/* SH4 EXPEVT code to string */
static const char *dc_exception_code_to_string(uint32_t code) {
    switch (code) {
    case 0x1e0: return "User break";
    case 0x0e0: return "Address error (read)";
    case 0x040: return "TLB miss exception (read)";
    case 0x0a0: return "TLB protection violation (read)";
    case 0x180: return "General illegal instruction";
    case 0x1a0: return "Slot illegal instruction";
    case 0x800: return "General FPU disable";
    case 0x820: return "Slot FPU disable";
    case 0x100: return "Address error (write)";
    case 0x060: return "TLB miss exception (write)";
    case 0x0c0: return "TLB protection violation (write)";
    case 0x120: return "FPU exception";
    case 0x080: return "Initial page write exception";
    case 0x160: return "Unconditional trap (TRAPA)";
    default:    return "Unknown exception";
    }
}

/* PPC 750 exception vector to string */
static const char *gc_exception_code_to_string(uint32_t code) {
    switch (code) {
    case 0x0100: return "System Reset";
    case 0x0200: return "Machine Check";
    case 0x0300: return "DSI (Data Storage)";
    case 0x0400: return "ISI (Instruction Storage)";
    case 0x0500: return "External Interrupt";
    case 0x0600: return "Alignment";
    case 0x0700: return "Program";
    case 0x0800: return "FP Unavailable";
    case 0x0900: return "Decrementer";
    case 0x0C00: return "System Call";
    case 0x0D00: return "Trace";
    case 0x0F00: return "Performance Monitor";
    case 0x1300: return "IABR";
    case 0x1700: return "Thermal";
    default:     return "Unknown Exception";
    }
}

static const char *dc_register_names[66] = {
    "PC", "PR", "SR", "GBR", "VBR", "DBR", "MACH", "MACL",
    "R0B0", "R1B0", "R2B0", "R3B0", "R4B0", "R5B0", "R6B0", "R7B0",
    "R0B1", "R1B1", "R2B1", "R3B1", "R4B1", "R5B1", "R6B1", "R7B1",
    "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15",
    "FPSCR",
    "FR0", "FR1", "FR2", "FR3", "FR4", "FR5", "FR6", "FR7",
    "FR8", "FR9", "FR10", "FR11", "FR12", "FR13", "FR14", "FR15",
    "FPUL",
    "XF0", "XF1", "XF2", "XF3", "XF4", "XF5", "XF6", "XF7",
    "XF8", "XF9", "XF10", "XF11", "XF12", "XF13", "XF14", "XF15",
};

static const char *gc_register_names[40] = {
    "SRR0", "SRR1",
    "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7",
    "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15",
    "R16", "R17", "R18", "R19", "R20", "R21", "R22", "R23",
    "R24", "R25", "R26", "R27", "R28", "R29", "R30", "R31",
    "LR", "CTR", "XER", "CR", "DSISR", "DAR",
};

static const char *gc_fpr_names[32] = {
    "F0",  "F1",  "F2",  "F3",  "F4",  "F5",  "F6",  "F7",
    "F8",  "F9",  "F10", "F11", "F12", "F13", "F14", "F15",
    "F16", "F17", "F18", "F19", "F20", "F21", "F22", "F23",
    "F24", "F25", "F26", "F27", "F28", "F29", "F30", "F31",
};

/* Swap bytes of a uint32 from little-endian to host order */
static uint32_t le32_to_host(uint32_t x) {
    /* If host is LE, no-op; if host is BE, swap */
    if (htonl(1) == 1)
        return (x << 24) | ((x << 8) & 0xff0000) |
               ((x >> 8) & 0xff00) | ((x >> 24) & 0xff);
    return x;
}

/* Swap bytes of a uint32 from big-endian to host order */
static uint32_t be32_to_host(uint32_t x) {
    return ntohl(x);
}

/* Generate timestamped exception dump filename */
static void make_dump_filename(const char *prefix, char *out, size_t out_size) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    snprintf(out, out_size, "%s_exception_%04d%02d%02d_%02d%02d%02d.txt",
             prefix,
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);
}

/* Print all registers to a FILE (stderr or dump file) */
static void print_dc_registers(FILE *fp, const uint32_t *regs) {
    for (int i = 0; i < 66; i++)
        fprintf(fp, "  %-6s 0x%08x\n", dc_register_names[i], regs[i]);
}

static void print_gc_registers(FILE *fp, const uint32_t *regs,
                                const uint8_t *data) {
    for (int i = 0; i < 40; i++)
        fprintf(fp, "  %-6s 0x%08x\n", gc_register_names[i], regs[i]);

    const int fpu_offset = 8 + 40 * sizeof(uint32_t);

    uint32_t fpscr_lo;
    memcpy(&fpscr_lo, data + fpu_offset + 4, sizeof(uint32_t));
    fpscr_lo = be32_to_host(fpscr_lo);
    fprintf(fp, "  FPSCR  0x%08x\n", fpscr_lo);

    for (int i = 0; i < 32; i++) {
        uint32_t hi, lo;
        memcpy(&hi, data + fpu_offset + 8 + i * 8, sizeof(uint32_t));
        memcpy(&lo, data + fpu_offset + 8 + i * 8 + 4, sizeof(uint32_t));
        hi = be32_to_host(hi);
        lo = be32_to_host(lo);
        fprintf(fp, "  %-6s 0x%08x%08x\n", gc_fpr_names[i], hi, lo);
    }
}

static void handle_dc_exception(kostool_context_t *ctx, const uint8_t *data,
                                uint32_t count) {
    if (count < sizeof(sh4_exception_frame_t)) {
        fprintf(stderr, "\nIncomplete DC exception frame (%u bytes)\n", count);
        return;
    }

    /* Byte-swap from SH4 (little-endian) to host order.
     * 66 registers start at offset 8 in the frame (after id + expt_code). */
    uint32_t regs[66];
    memcpy(regs, data + 8, 66 * sizeof(uint32_t));
    for (int i = 0; i < 66; i++)
        regs[i] = le32_to_host(regs[i]);

    uint32_t expt_code;
    memcpy(&expt_code, data + 4, sizeof(uint32_t));
    expt_code = le32_to_host(expt_code);

    const char *expt_str = dc_exception_code_to_string(expt_code);

    fprintf(stderr, "\n=== Dreamcast Exception: %s (0x%03x) ===\n",
            expt_str, expt_code);

    /* Always print key registers */
    fprintf(stderr, "  %-6s 0x%08x\n", "PC", regs[0]);
    fprintf(stderr, "  %-6s 0x%08x\n", "PR", regs[1]);
    fprintf(stderr, "  %-6s 0x%08x\n", "SR", regs[2]);
    fprintf(stderr, "  %-6s 0x%08x\n", "R15", regs[31]);

    /* addr2line on valid addresses, or fall back to full register dump */
    if (addr2line_available < 0) {
        addr2line_available = ctx->loaded_binary_path &&
                              ctx->sh4_addr2line[0] &&
                              access(ctx->sh4_addr2line, X_OK) == 0 &&
                              is_elf_file(ctx->loaded_binary_path);
#ifndef _WIN32
        if (addr2line_available)
            start_addr2line_process(ctx->sh4_addr2line, ctx->loaded_binary_path);
#endif
    }

    if (addr2line_available) {
        for (int i = 0; i < 66; i++) {
            if (regs[i] >= DC_DEFAULT_LOAD_ADDR && regs[i] < DC_RAM_TOP) {
                char decoded[256];
                decode_address(ctx->sh4_addr2line, ctx->loaded_binary_path,
                               regs[i], decoded, sizeof(decoded));
                if (decoded[0] && decoded[0] != '?')
                    fprintf(stderr, "  %-6s -> %s\n", dc_register_names[i], decoded);
            }
        }
    } else {
        print_dc_registers(stderr, regs);
    }

    /* Save full register dump to text file */
    char filename[128];
    make_dump_filename("dc", filename, sizeof(filename));
    FILE *dump = fopen(filename, "w");
    if (dump) {
        fprintf(dump, "=== Dreamcast Exception: %s (0x%03x) ===\n\n",
                expt_str, expt_code);
        fprintf(dump, "Registers:\n");
        print_dc_registers(dump, regs);
        if (addr2line_available) {
            fprintf(dump, "\nSymbols:\n");
            for (int i = 0; i < 66; i++) {
                if (regs[i] >= DC_DEFAULT_LOAD_ADDR && regs[i] < DC_RAM_TOP) {
                    char decoded[256];
                    decode_address(ctx->sh4_addr2line, ctx->loaded_binary_path,
                                   regs[i], decoded, sizeof(decoded));
                    if (decoded[0] && decoded[0] != '?')
                        fprintf(dump, "  %-6s -> %s\n", dc_register_names[i], decoded);
                }
            }
        }
        fclose(dump);
        fprintf(stderr, "  Saved to %s\n", filename);
    }
}

static void handle_gc_exception(kostool_context_t *ctx, const uint8_t *data,
                                uint32_t count) {
    if (count < sizeof(gc_exception_frame_t)) {
        fprintf(stderr, "\nIncomplete GC exception frame (%u bytes)\n", count);
        return;
    }

    /* Byte-swap from PPC (big-endian) to host order.
     * 40 GPR/SPR registers start at offset 8 (after id + expt_code):
     * SRR0, SRR1, R0-R31, LR, CTR, XER, CR, DSISR, DAR. */
    uint32_t regs[40];
    memcpy(regs, data + 8, 40 * sizeof(uint32_t));
    for (int i = 0; i < 40; i++)
        regs[i] = be32_to_host(regs[i]);

    uint32_t expt_code;
    memcpy(&expt_code, data + 4, sizeof(uint32_t));
    expt_code = be32_to_host(expt_code);

    const char *expt_str = gc_exception_code_to_string(expt_code);

    fprintf(stderr, "\n=== GameCube Exception: %s (0x%04x) ===\n",
            expt_str, expt_code);

    /* Always print key registers */
    fprintf(stderr, "  %-6s 0x%08x\n", "SRR0", regs[0]);
    fprintf(stderr, "  %-6s 0x%08x\n", "SRR1", regs[1]);
    fprintf(stderr, "  %-6s 0x%08x\n", "R1", regs[4]);
    fprintf(stderr, "  %-6s 0x%08x\n", "LR", regs[34]);

    /* For DSI/ISI exceptions, show the faulting address */
    if (expt_code == 0x0300 || expt_code == 0x0400) {
        fprintf(stderr, "  %-6s 0x%08x\n", "DAR", regs[39]);
        fprintf(stderr, "  %-6s 0x%08x\n", "DSISR", regs[38]);
    }

    /* addr2line on valid addresses, or fall back to full register dump */
    if (addr2line_available < 0) {
        addr2line_available = ctx->loaded_binary_path &&
                              ctx->ppc_addr2line[0] &&
                              access(ctx->ppc_addr2line, X_OK) == 0 &&
                              is_elf_file(ctx->loaded_binary_path);
#ifndef _WIN32
        if (addr2line_available)
            start_addr2line_process(ctx->ppc_addr2line, ctx->loaded_binary_path);
#endif
    }

    if (addr2line_available) {
        for (int i = 0; i < 40; i++) {
            if (regs[i] >= GC_DEFAULT_LOAD_ADDR && regs[i] < GC_RAM_TOP) {
                char decoded[256];
                decode_address(ctx->ppc_addr2line, ctx->loaded_binary_path,
                               regs[i], decoded, sizeof(decoded));
                if (decoded[0] && decoded[0] != '?')
                    fprintf(stderr, "  %-6s -> %s\n", gc_register_names[i], decoded);
            }
        }
    } else {
        print_gc_registers(stderr, regs, data);
    }

    /* Save full register dump to text file */
    char filename[128];
    make_dump_filename("gc", filename, sizeof(filename));
    FILE *dump = fopen(filename, "w");
    if (dump) {
        fprintf(dump, "=== GameCube Exception: %s (0x%04x) ===\n\n",
                expt_str, expt_code);
        fprintf(dump, "Registers:\n");
        print_gc_registers(dump, regs, data);
        if (addr2line_available) {
            fprintf(dump, "\nSymbols:\n");
            for (int i = 0; i < 40; i++) {
                if (regs[i] >= GC_DEFAULT_LOAD_ADDR && regs[i] < GC_RAM_TOP) {
                    char decoded[256];
                    decode_address(ctx->ppc_addr2line, ctx->loaded_binary_path,
                                   regs[i], decoded, sizeof(decoded));
                    if (decoded[0] && decoded[0] != '?')
                        fprintf(dump, "  %-6s -> %s\n", gc_register_names[i], decoded);
                }
            }
        }
        fclose(dump);
        fprintf(stderr, "  Saved to %s\n", filename);
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

#define GC_ENC_RETVAL_DELAY_USEC 5000

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
    if (ctx->installed_adapter == ADAPTER_GC_ENC &&
        memcmp(cmd, NET_CMD_RETVAL, 4) == 0 &&
        ctx->time_ops && ctx->time_ops->sleep_usec) {
        ctx->time_ops->sleep_usec(GC_ENC_RETVAL_DELAY_USEC);
    }
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

static void ser_syscall_fstat(kostool_context_t *ctx) {
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

static void ser_syscall_write(kostool_context_t *ctx) {
    int fd = ser_recv_uint(ctx);
    uint32_t count = ser_recv_uint(ctx);
    if (!count || count > MAX_SYSCALL_SIZE) { ser_send_uint(ctx, -1); return; }
    uint8_t *data = malloc(count);
    if (!data) { ser_send_uint(ctx, -1); return; }
    ser_recv_data(ctx, data, count);

    int ret;
    if (count >= 8 && !memcmp(data, KOSLOAD_EXCEPTION_TAG, 4)) {
        if (ctx->target_big_endian)
            handle_gc_exception(ctx, data, count);
        else
            handle_dc_exception(ctx, data, count);
        ret = (int)count;
    } else {
        ret = console_write(ctx, fd, data, count);
    }

    ser_send_uint(ctx, ret);
    free(data);
}

static void ser_syscall_read(kostool_context_t *ctx) {
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

static void ser_syscall_open(kostool_context_t *ctx) {
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

static void ser_syscall_close(kostool_context_t *ctx) {
    int fd = ser_recv_uint(ctx);
    int ret = close(fd);
    ser_send_uint(ctx, ret);
}

static void ser_syscall_creat(kostool_context_t *ctx) {
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

static void ser_syscall_link(kostool_context_t *ctx) {
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

static void ser_syscall_unlink(kostool_context_t *ctx) {
    uint32_t namelen = ser_recv_uint(ctx);
    if (!namelen || namelen > MAX_PATH_LEN) { ser_send_uint(ctx, -1); return; }
    char *pathname = malloc(namelen);
    if (!pathname) { ser_send_uint(ctx, -1); return; }
    ser_recv_data(ctx, pathname, namelen);
    int ret = unlink(pathname);
    ser_send_uint(ctx, ret);
    free(pathname);
}

static void ser_syscall_chdir(kostool_context_t *ctx) {
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

static void ser_syscall_chmod(kostool_context_t *ctx) {
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

static void ser_syscall_mkdir(kostool_context_t *ctx) {
    uint32_t namelen = ser_recv_uint(ctx);
    if (!namelen || namelen > MAX_PATH_LEN) { ser_send_uint(ctx, -1); return; }
    char *pathname = malloc(namelen);
    if (!pathname) { ser_send_uint(ctx, -1); return; }
    ser_recv_data(ctx, pathname, namelen);
    int mode = ser_recv_uint(ctx);
    char buf[MAX_PATH_LEN];
    const char *resolved = resolve_path(ctx, pathname, buf, sizeof(buf));
    int ret = host_mkdir(resolved, mode);
    ser_send_uint(ctx, ret);
    free(pathname);
}

static void ser_syscall_lseek(kostool_context_t *ctx) {
    int fd = ser_recv_uint(ctx);
    int offset = ser_recv_uint(ctx);
    int whence = ser_recv_uint(ctx);
    int ret = lseek(fd, offset, whence);
    ser_send_uint(ctx, ret);
}

static void ser_syscall_time(kostool_context_t *ctx) {
    time_t t;
    time(&t);
    ser_send_uint(ctx, (uint32_t)t);
}

static void ser_syscall_stat(kostool_context_t *ctx) {
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

static void ser_syscall_utime(kostool_context_t *ctx) {
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

static void ser_syscall_opendir(kostool_context_t *ctx) {
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

static void ser_syscall_closedir(kostool_context_t *ctx) {
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

static void ser_syscall_readdir(kostool_context_t *ctx) {
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

static void ser_syscall_rewinddir(kostool_context_t *ctx) {
    uint32_t i = ser_recv_uint(ctx);
    if (i >= DIRENT_OFFSET && i < MAX_OPEN_DIRS + DIRENT_OFFSET) {
        rewinddir(opendirs[i - DIRENT_OFFSET]);
    }
    ser_send_uint(ctx, 0);
}

static void ser_syscall_cdfs_read(kostool_context_t *ctx) {
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

static void gdb_probe_stream(int *in_packet, int *checksum_bytes,
                             size_t *payload_len, char *payload,
                             size_t payload_cap, const char *data, size_t len,
                             int *saw_detach, int *saw_ok) {
    size_t i;

    for (i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)data[i];

        if (!*in_packet) {
            if (ch == '$') {
                *in_packet = 1;
                *checksum_bytes = 0;
                *payload_len = 0;
            }
            continue;
        }

        if (ch == '$') {
            *checksum_bytes = 0;
            *payload_len = 0;
            continue;
        }

        if (*checksum_bytes) {
            if (++(*checksum_bytes) == 3) {
                payload[*payload_len] = '\0';

                if (saw_detach &&
                    payload[0] == 'D' &&
                    (payload[1] == '\0' || payload[1] == ';')) {
                    *saw_detach = 1;
                }

                if (saw_ok && payload[0] == 'O' && payload[1] == 'K' &&
                    payload[2] == '\0') {
                    *saw_ok = 1;
                }

                *in_packet = 0;
                *checksum_bytes = 0;
                *payload_len = 0;
            }
            continue;
        }

        if (ch == '#') {
            *checksum_bytes = 1;
            continue;
        }

        if (*payload_len + 1 < payload_cap)
            payload[(*payload_len)++] = (char)ch;
    }
}

static void gdb_note_client_bytes(kostool_context_t *ctx, const char *data, size_t len) {
    int saw_detach = 0;

    gdb_probe_stream(&ctx->gdb_client_probe.in_packet,
                     &ctx->gdb_client_probe.checksum_bytes,
                     &ctx->gdb_client_probe.payload_len,
                     ctx->gdb_client_probe.payload,
                     sizeof(ctx->gdb_client_probe.payload),
                     data, len, &saw_detach, NULL);

    if (saw_detach)
        ctx->gdb_detach_pending = 1;
}

static int gdb_note_target_bytes(kostool_context_t *ctx, const char *data, size_t len) {
    int saw_ok = 0;

    if (!ctx->gdb_detach_pending)
        return 0;

    gdb_probe_stream(&ctx->gdb_target_probe.in_packet,
                     &ctx->gdb_target_probe.checksum_bytes,
                     &ctx->gdb_target_probe.payload_len,
                     ctx->gdb_target_probe.payload,
                     sizeof(ctx->gdb_target_probe.payload),
                     data, len, NULL, &saw_ok);

    return saw_ok;
}

static void gdb_clear_detach_state(kostool_context_t *ctx) {
    ctx->gdb_detach_pending = 0;
    memset(&ctx->gdb_client_probe, 0, sizeof(ctx->gdb_client_probe));
    memset(&ctx->gdb_target_probe, 0, sizeof(ctx->gdb_target_probe));
}

static void ser_syscall_gdbpacket(kostool_context_t *ctx) {
    uint32_t in_size = ser_recv_uint(ctx);
    uint32_t out_size = ser_recv_uint(ctx);
    static char gdb_buf[1024];
    int retval = 0;
    int close_after_reply = 0;

    if (in_size)
        ser_recv_data(ctx, gdb_buf, in_size > 1024 ? 1024 : in_size);

    if (ctx->gdb_server_socket < 0) {
        ser_send_uint(ctx, (uint32_t)-1);
        return;
    }

    if (ctx->gdb_client_socket < 0) {
        printf("waiting for gdb client connection...\n");
        ctx->gdb_client_socket = ctx->socket_ops->accept(ctx->gdb_server_socket);
        if (ctx->gdb_client_socket < 0) {
            fprintf(stderr, "error accepting gdb connection\n");
            ser_send_uint(ctx, (uint32_t)-1);
            return;
        }
        printf("GDB client connected\n");
    }

    if (in_size) {
        close_after_reply = gdb_note_target_bytes(ctx, gdb_buf, in_size);

        if (gdb_send_all(ctx, ctx->gdb_client_socket, gdb_buf, in_size) < 0) {
            fprintf(stderr, "GDB socket error\n");
            gdb_close_client(ctx);
            gdb_clear_detach_state(ctx);
            retval = 0;
            ser_send_uint(ctx, (uint32_t)retval);
            return;
        }
    }

    if (out_size) {
        retval = ctx->socket_ops->recv(ctx->gdb_client_socket,
                                       gdb_buf, out_size > 1024 ? 1024 : out_size);
        if (retval == 0) {
            printf("GDB client disconnected\n");
            gdb_close_client(ctx);
            gdb_clear_detach_state(ctx);
        }
        else if (retval > 0)
            gdb_note_client_bytes(ctx, gdb_buf, (size_t)retval);
    }
    if (retval < 0) {
        fprintf(stderr, "GDB socket error\n");
        gdb_close_client(ctx);
        gdb_clear_detach_state(ctx);
        retval = 0;
    }

    if (close_after_reply) {
        printf("GDB client detached\n");
        gdb_close_client(ctx);
        gdb_clear_detach_state(ctx);
    }

    ser_send_uint(ctx, retval);
    if (retval > 0)
        ser_send_data(ctx, (const uint8_t *)gdb_buf, retval);
}

static int ser_syscall_exit(kostool_context_t *ctx) {
    return (int32_t)ser_recv_uint(ctx);
}

static int ser_try_recv_uint(kostool_context_t *ctx, uint32_t *value,
                             uint32_t timeout_usec) {
    uint64_t deadline = 0;

    if (!ctx->serial_ops->bytes_available || !value)
        return 0;

    if (ctx->time_ops)
        deadline = ctx->time_ops->time_usec() + timeout_usec;

    while (1) {
        int available = ctx->serial_ops->bytes_available(ctx->serial_handle);

        if (available >= 4) {
            *value = ser_recv_uint(ctx);
            return 1;
        }

        if (available < 0)
            return 0;

        if (!ctx->time_ops || ctx->time_ops->time_usec() >= deadline)
            return 0;

        ctx->time_ops->sleep_usec(1000);
    }
}

/* ===== Network console syscall handlers ===== */

static void net_syscall_fstat(kostool_context_t *ctx, uint8_t *pkt) {
    net_command_3int_t *cmd = (net_command_3int_t *)pkt;
    int fd = ntohl(cmd->value0);
    uint32_t addr = ntohl(cmd->value1);
    uint32_t sz = ntohl(cmd->value2);
    struct stat st = {0};
    int ret = fstat(fd, &st);
    kosload_stat_t ds = {0};
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

static void net_syscall_write(kostool_context_t *ctx, uint8_t *pkt) {
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

    int ret;
    if (count >= 8 && !memcmp(data, KOSLOAD_EXCEPTION_TAG, 4)) {
        if (ctx->target_big_endian)
            handle_gc_exception(ctx, data, count);
        else
            handle_dc_exception(ctx, data, count);
        ret = (int)count;
    } else {
        ret = console_write(ctx, fd, data, count);
    }

    net_send_cmd(ctx, NET_CMD_RETVAL, ret, ret, NULL, 0);
    free(data);
}

static void net_syscall_read(kostool_context_t *ctx, uint8_t *pkt) {
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

static void net_syscall_open(kostool_context_t *ctx, uint8_t *pkt) {
    net_command_2int_string_t *cmd = (net_command_2int_string_t *)pkt;
    int flags = ntohl(cmd->value0);
    int mode = ntohl(cmd->value1);
    int ourflags = ctx->fs_ops->translate_open_flags(flags);
    char buf[MAX_PATH_LEN];
    const char *resolved = resolve_path(ctx, cmd->string, buf, sizeof(buf));
    int ret = open(resolved, ourflags | O_BINARY, mode);
    net_send_cmd(ctx, NET_CMD_RETVAL, ret, ret, NULL, 0);
}

static void net_syscall_close(kostool_context_t *ctx, uint8_t *pkt) {
    net_command_int_t *cmd = (net_command_int_t *)pkt;
    int ret = close(ntohl(cmd->value0));
    net_send_cmd(ctx, NET_CMD_RETVAL, ret, ret, NULL, 0);
}

static void net_syscall_creat(kostool_context_t *ctx, uint8_t *pkt) {
    net_command_int_string_t *cmd = (net_command_int_string_t *)pkt;
    char buf[MAX_PATH_LEN];
    const char *resolved = resolve_path(ctx, cmd->string, buf, sizeof(buf));
    int ret = creat(resolved, ntohl(cmd->value0));
    net_send_cmd(ctx, NET_CMD_RETVAL, ret, ret, NULL, 0);
}

static void net_syscall_link(kostool_context_t *ctx, uint8_t *pkt) {
    net_command_string_t *cmd = (net_command_string_t *)pkt;
    const char *path1 = cmd->string;
    const char *path2 = path1 + strlen(path1) + 1;
    int ret = link(path1, path2);
    net_send_cmd(ctx, NET_CMD_RETVAL, ret, ret, NULL, 0);
}

static void net_syscall_unlink(kostool_context_t *ctx, uint8_t *pkt) {
    net_command_string_t *cmd = (net_command_string_t *)pkt;
    int ret = unlink(cmd->string);
    net_send_cmd(ctx, NET_CMD_RETVAL, ret, ret, NULL, 0);
}

static void net_syscall_chdir(kostool_context_t *ctx, uint8_t *pkt) {
    net_command_string_t *cmd = (net_command_string_t *)pkt;
    char buf[MAX_PATH_LEN];
    const char *resolved = resolve_path(ctx, cmd->string, buf, sizeof(buf));
    int ret = chdir(resolved);
    net_send_cmd(ctx, NET_CMD_RETVAL, ret, ret, NULL, 0);
}

static void net_syscall_chmod(kostool_context_t *ctx, uint8_t *pkt) {
    net_command_int_string_t *cmd = (net_command_int_string_t *)pkt;
    char buf[MAX_PATH_LEN];
    const char *resolved = resolve_path(ctx, cmd->string, buf, sizeof(buf));
    int ret = chmod(resolved, ntohl(cmd->value0));
    net_send_cmd(ctx, NET_CMD_RETVAL, ret, ret, NULL, 0);
}

static void net_syscall_mkdir(kostool_context_t *ctx, uint8_t *pkt) {
    net_command_int_string_t *cmd = (net_command_int_string_t *)pkt;
    char buf[MAX_PATH_LEN];
    const char *resolved = resolve_path(ctx, cmd->string, buf, sizeof(buf));
    int ret = host_mkdir(resolved, ntohl(cmd->value0));
    net_send_cmd(ctx, NET_CMD_RETVAL, ret, ret, NULL, 0);
}

static void net_syscall_lseek(kostool_context_t *ctx, uint8_t *pkt) {
    net_command_3int_t *cmd = (net_command_3int_t *)pkt;
    int ret = lseek(ntohl(cmd->value0), ntohl(cmd->value1), ntohl(cmd->value2));
    net_send_cmd(ctx, NET_CMD_RETVAL, ret, ret, NULL, 0);
}

static void net_syscall_time(kostool_context_t *ctx, uint8_t *pkt) {
    (void)pkt;
    time_t t = time(NULL);
    net_send_cmd(ctx, NET_CMD_RETVAL, (uint32_t)t, (uint32_t)t, NULL, 0);
}

static void net_syscall_stat(kostool_context_t *ctx, uint8_t *pkt) {
    net_command_2int_string_t *cmd = (net_command_2int_string_t *)pkt;
    uint32_t addr = ntohl(cmd->value0);
    uint32_t sz = ntohl(cmd->value1);
    struct stat st = {0};
    char buf[MAX_PATH_LEN];
    const char *resolved = resolve_path(ctx, cmd->string, buf, sizeof(buf));
    int ret = stat(resolved, &st);
    kosload_stat_t ds = {0};
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

static void net_syscall_utime(kostool_context_t *ctx, uint8_t *pkt) {
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

static void net_syscall_opendir(kostool_context_t *ctx, uint8_t *pkt) {
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

static void net_syscall_closedir(kostool_context_t *ctx, uint8_t *pkt) {
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

static void net_syscall_readdir(kostool_context_t *ctx, uint8_t *pkt) {
    net_command_3int_t *cmd = (net_command_3int_t *)pkt;
    uint32_t i = ntohl(cmd->value0);
    uint32_t addr = ntohl(cmd->value1);
    uint32_t sz = ntohl(cmd->value2);
    struct dirent *de = NULL;
    if (i >= DIRENT_OFFSET && i < MAX_OPEN_DIRS + DIRENT_OFFSET)
        de = readdir(opendirs[i - DIRENT_OFFSET]);
    if (de) {
        kosload_dirent_t dd = {0};
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
        compat_str_copy(dd.d_name, sizeof(dd.d_name), de->d_name);
        ctx->transport->send_data(ctx, (uint8_t *)&dd, addr, sz);
        net_send_cmd(ctx, NET_CMD_RETVAL, 1, 1, NULL, 0);
    } else {
        net_send_cmd(ctx, NET_CMD_RETVAL, 0, 0, NULL, 0);
    }
}

static void net_syscall_rewinddir(kostool_context_t *ctx, uint8_t *pkt) {
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

static void net_syscall_cdfs_read(kostool_context_t *ctx, uint8_t *pkt) {
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

static void net_syscall_gdbpacket(kostool_context_t *ctx, uint8_t *pkt) {
    net_command_2int_string_t *cmd = (net_command_2int_string_t *)pkt;
    uint32_t in_size = ntohl(cmd->value0);
    uint32_t out_size = ntohl(cmd->value1);
    static char gdb_buf[1024];
    int retval = 0;
    int close_after_reply = 0;

    if (ctx->gdb_server_socket < 0) {
        net_send_cmd(ctx, NET_CMD_RETVAL, (uint32_t)-1, (uint32_t)-1, NULL, 0);
        return;
    }

    if (ctx->gdb_client_socket < 0) {
        printf("waiting for gdb client connection...\n");
        ctx->gdb_client_socket = ctx->socket_ops->accept(ctx->gdb_server_socket);
        if (ctx->gdb_client_socket < 0) {
            fprintf(stderr, "error accepting gdb connection\n");
            net_send_cmd(ctx, NET_CMD_RETVAL, (uint32_t)-1, (uint32_t)-1, NULL, 0);
            return;
        }
        printf("GDB client connected\n");
    }

    if (in_size) {
        close_after_reply = gdb_note_target_bytes(ctx, cmd->string, in_size);

        if (gdb_send_all(ctx, ctx->gdb_client_socket, cmd->string, in_size) < 0) {
            fprintf(stderr, "GDB socket error\n");
            gdb_close_client(ctx);
            gdb_clear_detach_state(ctx);
            net_send_cmd(ctx, NET_CMD_RETVAL, 0, 0, NULL, 0);
            return;
        }
    }

    if (out_size) {
        retval = ctx->socket_ops->recv(ctx->gdb_client_socket,
                                       gdb_buf, out_size > 1024 ? 1024 : out_size);
        if (retval == 0) {
            printf("GDB client disconnected\n");
            gdb_close_client(ctx);
            gdb_clear_detach_state(ctx);
        }
        else if (retval > 0)
            gdb_note_client_bytes(ctx, gdb_buf, (size_t)retval);
    }
    if (retval < 0) {
        fprintf(stderr, "GDB socket error\n");
        gdb_close_client(ctx);
        gdb_clear_detach_state(ctx);
        retval = 0;
    }

    if (close_after_reply) {
        printf("GDB client detached\n");
        gdb_close_client(ctx);
        gdb_clear_detach_state(ctx);
    }

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

    // if (ctx->use_chroot && ctx->chroot_path) {
    //     ctx->fs_ops->chroot(ctx->chroot_path);
    // }

    while (1) {
        fflush(stdout);
        uint8_t command;
        ser_blread(ctx, &command, 1);

        switch (command) {
        case SERIAL_SYSCALL_EXIT: {
            uint32_t ret_code = 0;

            if (ser_try_recv_uint(ctx, &ret_code, SERIAL_EXIT_CODE_PROBE_USEC))
                printf("Program returned %d\n", (int32_t)ret_code);

            gdb_report_program_exit(ctx, (int32_t)ret_code);
            exit(0);
            break;
        }
        case SERIAL_SYSCALL_FSTAT:     ser_syscall_fstat(ctx); break;
        case SERIAL_SYSCALL_WRITE:     ser_syscall_write(ctx); break;
        case SERIAL_SYSCALL_READ:      ser_syscall_read(ctx); break;
        case SERIAL_SYSCALL_OPEN:      ser_syscall_open(ctx); break;
        case SERIAL_SYSCALL_CLOSE:     ser_syscall_close(ctx); break;
        case SERIAL_SYSCALL_CREAT:     ser_syscall_creat(ctx); break;
        case SERIAL_SYSCALL_LINK:      ser_syscall_link(ctx); break;
        case SERIAL_SYSCALL_UNLINK:    ser_syscall_unlink(ctx); break;
        case SERIAL_SYSCALL_CHDIR:     ser_syscall_chdir(ctx); break;
        case SERIAL_SYSCALL_CHMOD:     ser_syscall_chmod(ctx); break;
        case SERIAL_SYSCALL_LSEEK:     ser_syscall_lseek(ctx); break;
        case SERIAL_SYSCALL_TIME:      ser_syscall_time(ctx); break;
        case SERIAL_SYSCALL_STAT:      ser_syscall_stat(ctx); break;
        case SERIAL_SYSCALL_UTIME:     ser_syscall_utime(ctx); break;
        case SERIAL_SYSCALL_BAD:
            printf("command 15 should not happen... (but it did)\n");
            break;
        case SERIAL_SYSCALL_OPENDIR:   ser_syscall_opendir(ctx); break;
        case SERIAL_SYSCALL_CLOSEDIR:  ser_syscall_closedir(ctx); break;
        case SERIAL_SYSCALL_READDIR:   ser_syscall_readdir(ctx); break;
        case SERIAL_SYSCALL_CDFSREAD:  ser_syscall_cdfs_read(ctx); break;
        case SERIAL_SYSCALL_GDBPACKET: ser_syscall_gdbpacket(ctx); break;
        case SERIAL_SYSCALL_REWINDDIR: ser_syscall_rewinddir(ctx); break;
        case SERIAL_SYSCALL_MKDIR:     ser_syscall_mkdir(ctx); break;
        case SERIAL_SYSCALL_PROGEXIT: {
            int32_t ret_code = ser_syscall_exit(ctx);
            printf("Program returned %d\n", ret_code);
            gdb_report_program_exit(ctx, ret_code);
            exit(0);
        }
        default:
            printf("Unimplemented command (%d)\n", command);
            printf("Assuming program has exited\n");
            exit(-1);
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

    while (1) {
        fflush(stdout);

        while (net_recv_resp(ctx, buffer, sizeof(buffer), NET_PACKET_TIMEOUT_USEC) == -1)
            ;
        /* Guarantee null termination so strlen() on string commands
         * can never walk past the buffer. */
        buffer[sizeof(buffer) - 1] = '\0';

        if (!memcmp(buffer, NET_SYSCALL_EXIT, 4) || !memcmp(buffer, NET_SYSCALL_PROGEXIT, 4)) {
            net_command_t *exit_cmd = (net_command_t *)buffer;
            int32_t ret_code = (int32_t)ntohl(exit_cmd->address);
            printf("Program returned %d\n", ret_code);
            gdb_report_program_exit(ctx, ret_code);
            return 0;
        }
        if (!memcmp(buffer, NET_SYSCALL_FSTAT, 4))      net_syscall_fstat(ctx, buffer);
        else if (!memcmp(buffer, NET_SYSCALL_WRITE, 4))  net_syscall_write(ctx, buffer);
        else if (!memcmp(buffer, "DD02", 4))         net_syscall_write(ctx, buffer); /* legacy */
        else if (!memcmp(buffer, NET_SYSCALL_READ, 4))   net_syscall_read(ctx, buffer);
        else if (!memcmp(buffer, NET_SYSCALL_OPEN, 4))   net_syscall_open(ctx, buffer);
        else if (!memcmp(buffer, NET_SYSCALL_CLOSE, 4))  net_syscall_close(ctx, buffer);
        else if (!memcmp(buffer, NET_SYSCALL_CREAT, 4))  net_syscall_creat(ctx, buffer);
        else if (!memcmp(buffer, NET_SYSCALL_LINK, 4))   net_syscall_link(ctx, buffer);
        else if (!memcmp(buffer, NET_SYSCALL_UNLINK, 4)) net_syscall_unlink(ctx, buffer);
        else if (!memcmp(buffer, NET_SYSCALL_CHDIR, 4))  net_syscall_chdir(ctx, buffer);
        else if (!memcmp(buffer, NET_SYSCALL_CHMOD, 4))  net_syscall_chmod(ctx, buffer);
        else if (!memcmp(buffer, NET_SYSCALL_LSEEK, 4))  net_syscall_lseek(ctx, buffer);
        else if (!memcmp(buffer, NET_SYSCALL_TIME, 4))   net_syscall_time(ctx, buffer);
        else if (!memcmp(buffer, NET_SYSCALL_STAT, 4))   net_syscall_stat(ctx, buffer);
        else if (!memcmp(buffer, NET_SYSCALL_UTIME, 4))  net_syscall_utime(ctx, buffer);
        else if (!memcmp(buffer, NET_SYSCALL_BAD, 4))
            fprintf(stderr, "command 15 should not happen... (but it did)\n");
        else if (!memcmp(buffer, NET_SYSCALL_OPENDIR, 4))   net_syscall_opendir(ctx, buffer);
        else if (!memcmp(buffer, NET_SYSCALL_CLOSEDIR, 4))  net_syscall_closedir(ctx, buffer);
        else if (!memcmp(buffer, NET_SYSCALL_READDIR, 4))   net_syscall_readdir(ctx, buffer);
        else if (!memcmp(buffer, NET_SYSCALL_CDFSREAD, 4))  net_syscall_cdfs_read(ctx, buffer);
        else if (!memcmp(buffer, NET_SYSCALL_GDBPACKET, 4)) net_syscall_gdbpacket(ctx, buffer);
        else if (!memcmp(buffer, NET_SYSCALL_REWINDDIR, 4)) net_syscall_rewinddir(ctx, buffer);
        else if (!memcmp(buffer, NET_SYSCALL_MKDIR, 4))     net_syscall_mkdir(ctx, buffer);
    }

    return 0;
}

/* ===== Main console entry point ===== */

int do_console(kostool_context_t *ctx) {
    /* Set up GDB server if requested */
    if (ctx->gdb_enabled && ctx->gdb_server_socket < 0) {
        if (gdb_init(ctx, NET_GDB_PORT) != 0)
            return 1;
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
