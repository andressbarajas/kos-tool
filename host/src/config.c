/* host/src/config.c */
/*
 * Configuration file support for kos-tool.
 *
 * On Linux/BSD the XDG location is preferred: kos-tool.cfg is read from
 * $XDG_CONFIG_HOME (or ~/.config) first, then from the directory holding
 * the kos-tool binary.  If neither exists a default is created under
 * ~/.config.  On macOS and Windows the config is read/created next to the
 * binary, as before.
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

/* The XDG Base Directory convention (~/.config) is a Linux/BSD thing; macOS
 * and Windows keep the legacy "next to the binary" behavior. */
#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)
#define KOSTOOL_XDG_CONFIG 1
#include <sys/stat.h>
#include <sys/types.h>
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

#ifdef KOSTOOL_XDG_CONFIG
/* Build the XDG config path ($XDG_CONFIG_HOME/kos-tool.cfg, or
 * ~/.config/kos-tool.cfg) into `path`, and the containing directory into
 * `dir` (so the caller can create it on first run).  Returns 0 on success,
 * -1 if neither $XDG_CONFIG_HOME nor $HOME is usable. */
static int get_xdg_config_path(char *path, size_t path_sz, char *dir, size_t dir_sz) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if(xdg && xdg[0]) {
        compat_str_copy(dir, dir_sz, xdg);
    } else {
        const char *home = getenv("HOME");
        if(!home || !home[0])
            return -1;
        snprintf(dir, dir_sz, "%s/.config", home);
    }
    compat_path_join(path, path_sz, dir, CONFIG_FILENAME);
    return 0;
}
#endif

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

    FILE *fp = NULL;

#ifdef KOSTOOL_XDG_CONFIG
    char xdg_dir[4096];
    char xdg_path[4096];
    int have_xdg = (get_xdg_config_path(xdg_path, sizeof(xdg_path), xdg_dir, sizeof(xdg_dir)) == 0);

    /* 1. Prefer the XDG config (~/.config) if it exists. */
    if(have_xdg) {
        fp = fopen(xdg_path, "r");
        if(fp)
            compat_str_copy(ctx->config_path, sizeof(ctx->config_path), xdg_path);
    }

    /* 2. Otherwise fall back to a config next to the binary, if present. */
    if(!fp && get_executable_dir(dir, sizeof(dir)) == 0) {
        compat_path_join(path, sizeof(path), dir, CONFIG_FILENAME);
        fp = fopen(path, "r");
        if(fp)
            compat_str_copy(ctx->config_path, sizeof(ctx->config_path), path);
    }

    /* 3. Neither exists — create a default under ~/.config. */
    if(!fp) {
        if(have_xdg) {
            mkdir(xdg_dir, 0755); /* harmless if it already exists */
            compat_str_copy(ctx->config_path, sizeof(ctx->config_path), xdg_path);
            FILE *out = fopen(xdg_path, "w");
            if(out) {
                fputs(default_config, out);
                fclose(out);
            }
        }
        return;
    }
#else
    /* macOS / Windows: config lives next to the binary. */
    if(get_executable_dir(dir, sizeof(dir)) != 0)
        return;

    compat_path_join(path, sizeof(path), dir, CONFIG_FILENAME);
    compat_str_copy(ctx->config_path, sizeof(ctx->config_path), path);

    fp = fopen(path, "r");
    if(!fp) {
        /* Create default config */
        fp = fopen(path, "w");
        if(fp) {
            fputs(default_config, fp);
            fclose(fp);
        }
        return;
    }
#endif

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
