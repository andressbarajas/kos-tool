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

#include <kostool/context.h>
#include <kostool/config.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifdef _WIN32
#include <windows.h>
#endif

#define CONFIG_FILENAME "kos-tool.cfg"

static const char default_config[] =
    "# kos-tool configuration\n"
    "#\n"
    "# Full paths to addr2line for each target architecture.\n"
    "# Used for exception register decoding and address annotation.\n"
    "\n"
    "sh4_addr2line = /opt/toolchains/dc/sh-elf/bin/sh-elf-addr2line\n" 
    "ppc_addr2line = /opt/toolchains/gc/powerpc-eabi/bin/powerpc-eabi-addr2line\n";

/* Get the directory containing the kos-tool binary. */
static int get_executable_dir(char *buf, size_t size)
{
#if defined(__APPLE__)
    uint32_t bufsize = (uint32_t)size;
    if (_NSGetExecutablePath(buf, &bufsize) != 0)
        return -1;
    /* Resolve symlinks */
    char resolved[4096];
    if (realpath(buf, resolved)) {
        strncpy(buf, resolved, size);
        buf[size - 1] = '\0';
    }
#elif defined(__linux__)
    ssize_t len = readlink("/proc/self/exe", buf, size - 1);
    if (len < 0) return -1;
    buf[len] = '\0';
#elif defined(_WIN32)
    DWORD len = GetModuleFileNameA(NULL, buf, (DWORD)size);
    if (len == 0) return -1;
    buf[len] = '\0';
#else
    return -1;
#endif

    /* Strip filename to get directory */
    char *sep = strrchr(buf, '/');
#ifdef _WIN32
    if (!sep) sep = strrchr(buf, '\\');
#endif
    if (sep)
        *sep = '\0';
    else
        return -1;

    return 0;
}

static void trim_whitespace(char *s)
{
    /* Leading */
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);

    /* Trailing */
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                       s[len - 1] == '\r' || s[len - 1] == '\n'))
        s[--len] = '\0';
}

static void apply_config_value(struct kostool_context *ctx,
                               const char *key, const char *value)
{
    if (strcmp(key, "sh4_addr2line") == 0) {
        strncpy(ctx->sh4_addr2line, value, sizeof(ctx->sh4_addr2line) - 1);
        ctx->sh4_addr2line[sizeof(ctx->sh4_addr2line) - 1] = '\0';
    } else if (strcmp(key, "ppc_addr2line") == 0) {
        strncpy(ctx->ppc_addr2line, value, sizeof(ctx->ppc_addr2line) - 1);
        ctx->ppc_addr2line[sizeof(ctx->ppc_addr2line) - 1] = '\0';
    }
    /* Unknown keys are silently ignored for forward compat */
}

void config_load(struct kostool_context *ctx)
{
    char dir[4096];
    char path[4096];

    /* Set defaults first (full paths) */
    strncpy(ctx->sh4_addr2line, "/opt/toolchains/dc/sh-elf/bin/sh-elf-addr2line", sizeof(ctx->sh4_addr2line));
    strncpy(ctx->ppc_addr2line, "/opt/toolchains/gc/powerpc-eabi/bin/powerpc-eabi-addr2line", sizeof(ctx->ppc_addr2line));

    if (get_executable_dir(dir, sizeof(dir)) != 0)
        return;

    snprintf(path, sizeof(path), "%s/%s", dir, CONFIG_FILENAME);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        /* Create default config */
        fp = fopen(path, "w");
        if (fp) {
            fputs(default_config, fp);
            fclose(fp);
        }
        return;
    }

    /* Parse config file */
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        trim_whitespace(line);

        /* Skip empty lines and comments */
        if (line[0] == '\0' || line[0] == '#')
            continue;

        char *eq = strchr(line, '=');
        if (!eq)
            continue;

        *eq = '\0';
        char *key = line;
        char *value = eq + 1;
        trim_whitespace(key);
        trim_whitespace(value);

        if (key[0] && value[0])
            apply_config_value(ctx, key, value);
    }

    fclose(fp);
}
