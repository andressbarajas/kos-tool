/* host/src/config.c */
/*
 * Configuration file support for kos-tool.
 *
 * Loads kos-tool.cfg from the same directory as the kos-tool binary.
 * Creates a default config if the file doesn't exist.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <kosload/strutil.h>
#include <kostool/context.h>
#include <kostool/config.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifdef _WIN32
#include <windows.h>
#endif

#define CONFIG_FILENAME "kos-tool.cfg"

/* addr2line defaults + tool prefixes are normally injected by the Makefile
 * from mk/toolchains.mk.  These fallbacks only apply to ad-hoc builds that
 * don't pass them. */

#ifndef SH4_TOOL_PREFIX
#define SH4_TOOL_PREFIX "sh-elf-"
#endif

#ifndef DEFAULT_SH4_ADDR2LINE
#define DEFAULT_SH4_ADDR2LINE "/opt/toolchains/dc/sh-elf/bin/sh-elf-addr2line"
#endif

#ifndef PPC_TOOL_PREFIX
#define PPC_TOOL_PREFIX "powerpc-eabi-"
#endif

#ifndef DEFAULT_PPC_ADDR2LINE
#define DEFAULT_PPC_ADDR2LINE "/opt/toolchains/gc/powerpc-eabi/bin/powerpc-eabi-addr2line"
#endif

#ifndef MIPS_TOOL_PREFIX
#define MIPS_TOOL_PREFIX "mips64r5900el-ps2-elf-"
#endif

#ifndef DEFAULT_MIPS_ADDR2LINE
#define DEFAULT_MIPS_ADDR2LINE "/opt/toolchains/ps2/mips-elf/bin/mips64r5900el-ps2-elf-addr2line"
#endif


static const char default_config[] =
    "# kos-tool configuration\n"
    "#\n"
    "# Target profiles for -T <profile>.\n"
    "# Uncomment and edit the entries you want to use.\n"
    "#\n"
    "# dc_serial = /dev/ttyUSB0\n"
    "# gc_serial = /dev/ttyUSB1\n"
    "# dc_ip = 172.16.0.10\n"
    "# gc_ip = dhcp\n"
    "# ps2_ip = dhcp\n"
    "# wii_ip = dhcp\n"
    "# serial_baud = 1562500\n";

/* Get the directory containing the kos-tool binary. */
static int get_executable_dir(char *buf, size_t size) {
#if defined(__APPLE__)
    uint32_t bufsize = (uint32_t)size;
    if(_NSGetExecutablePath(buf, &bufsize) != 0)
        return -1;
    /* Resolve symlinks */
    char resolved[4096];
    if(realpath(buf, resolved))
        compat_str_copy(buf, size, resolved);
#elif defined(__linux__)
    ssize_t len = readlink("/proc/self/exe", buf, size - 1);
    if(len < 0)
        return -1;
    buf[len] = '\0';
#elif defined(_WIN32)
    DWORD len = GetModuleFileNameA(NULL, buf, (DWORD)size);
    if(len == 0)
        return -1;
    buf[len] = '\0';
#else
    return -1;
#endif

    /* Strip filename to get directory */
    char *sep = strrchr(buf, '/');
#ifdef _WIN32
    if(!sep)
        sep = strrchr(buf, '\\');
#endif
    if(sep)
        *sep = '\0';
    else
        return -1;

    return 0;
}

static void trim_whitespace(char *s) {
    /* Leading */
    char *p = s;
    while(*p == ' ' || *p == '\t')
        p++;
    if(p != s)
        memmove(s, p, strlen(p) + 1);

    /* Trailing */
    size_t len = strlen(s);
    while(len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n'))
        s[--len] = '\0';
}

static void apply_config_value(struct kostool_context *ctx, const char *key, const char *value) {
    /* addr2line paths are derived (see config_load), not config-file keys. */
    if(strcmp(key, "dc_serial") == 0) {
        compat_str_copy(ctx->dc_serial, sizeof(ctx->dc_serial), value);
    } else if(strcmp(key, "gc_serial") == 0) {
        compat_str_copy(ctx->gc_serial, sizeof(ctx->gc_serial), value);
    } else if(strcmp(key, "dc_ip") == 0) {
        compat_str_copy(ctx->dc_ip, sizeof(ctx->dc_ip), value);
    } else if(strcmp(key, "gc_ip") == 0) {
        compat_str_copy(ctx->gc_ip, sizeof(ctx->gc_ip), value);
    } else if(strcmp(key, "ps2_ip") == 0) {
        compat_str_copy(ctx->ps2_ip, sizeof(ctx->ps2_ip), value);
    } else if(strcmp(key, "wii_ip") == 0) {
        compat_str_copy(ctx->wii_ip, sizeof(ctx->wii_ip), value);
    } else if(strcmp(key, "serial_baud") == 0) {
        uint32_t baud = (uint32_t)strtoul(value, NULL, 0);
        if(baud)
            ctx->serial_baud = baud;
    }
    /* Unknown keys are silently ignored for forward compat */
}

/* Build "<bindir>/<prefix>addr2line" from $envvar if it is set in the tool's
 * runtime environment, else fall back to the build-time default (which itself
 * was derived from mk/toolchains.mk). */
static void derive_addr2line(char *dst, size_t dstsz, const char *envvar, const char *prefix,
                             const char *fallback) {
    const char *base = getenv(envvar);
    if(base && base[0])
        snprintf(dst, dstsz, "%s/%saddr2line", base, prefix);
    else
        compat_str_copy(dst, dstsz, fallback);
}

void config_load(struct kostool_context *ctx) {
    char dir[4096];
    char path[4096];

    /* addr2line paths are derived, not user-configurable: $*_TOOLCHAIN env var
     * if present, else the build-time default (from mk/toolchains.mk). */
    derive_addr2line(ctx->sh4_addr2line, sizeof(ctx->sh4_addr2line), "DC_TOOLCHAIN", SH4_TOOL_PREFIX,
                     DEFAULT_SH4_ADDR2LINE);
    derive_addr2line(ctx->ppc_addr2line, sizeof(ctx->ppc_addr2line), "GC_TOOLCHAIN", PPC_TOOL_PREFIX,
                     DEFAULT_PPC_ADDR2LINE);
    derive_addr2line(ctx->mips_addr2line, sizeof(ctx->mips_addr2line), "PS2_EE_TOOLCHAIN", MIPS_TOOL_PREFIX,
                     DEFAULT_MIPS_ADDR2LINE);

    if(get_executable_dir(dir, sizeof(dir)) != 0)
        return;

    compat_path_join(path, sizeof(path), dir, CONFIG_FILENAME);
    compat_str_copy(ctx->config_path, sizeof(ctx->config_path), path);

    FILE *fp = fopen(path, "r");
    if(!fp) {
        /* Create default config */
        fp = fopen(path, "w");
        if(fp) {
            fputs(default_config, fp);
            fclose(fp);
        }
        return;
    }

    /* Parse config file */
    char line[1024];
    while(fgets(line, sizeof(line), fp)) {
        trim_whitespace(line);

        /* Skip empty lines and comments */
        if(line[0] == '\0' || line[0] == '#')
            continue;

        char *eq = strchr(line, '=');
        if(!eq)
            continue;

        *eq = '\0';
        char *key = line;
        char *value = eq + 1;
        trim_whitespace(key);
        trim_whitespace(value);

        if(key[0] && value[0])
            apply_config_value(ctx, key, value);
    }

    fclose(fp);
}
